/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <string.h>

#ifdef __linux__
#include <sys/resource.h>
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>

#include "osdep/io.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "stream/stream.h"
#include "mpv_talloc.h"
#include "common/av_common.h"
#include "common/msg.h"
#include "common/playlist.h"
#include "command.h"
#include "core.h"
#include "demux/demux.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "input/cmd.h"
#include "mpv/client.h"
#include "misc/node.h"
#include "misc/path_utils.h"
#include "misc/thread_tools.h"
#include "options/options.h"
#include "options/path.h"
#include "video/mp_image.h"
#include "video/sws_utils.h"
#include "thumbnail.h"

enum { SLOT_EMPTY, SLOT_DONE, SLOT_FAILED };

static const char magic[] = "mpvthmb1";

struct cache_header {
    char magic[8];
    uint64_t key;
    int32_t w, h, count;
    double step;
};

struct thumbnailer {
    struct mpv_global *global;
    struct mp_log *log;
    char *url;
    int stream_flags;
    bool rebase;
    int width;
    double interval;
    int max_count;
    bool sweep;
    char *cache_dir;
    char *headers;
    double delay;
    int hwdec;
    void (*wakeup)(void *ctx);
    void *wakeup_ctx;

    struct mp_cancel *cancel;
    mp_thread thread;
    mp_mutex lock;
    mp_cond cond;

    bool quit;
    bool finished;
    bool hold;
    bool eager;
    int want;
    int hover;
    int dir;
    bool ready;
    bool changed;
    int w, h, count;
    double step;
    uint8_t *state;
    uint8_t **slots;
    int done;

    FILE *cache;
    size_t slot_size;
    struct mp_sws_context *out_sws;
};

static uint64_t fnv(uint64_t h, const void *p, size_t size)
{
    const uint8_t *b = p;
    for (size_t n = 0; n < size; n++)
        h = (h ^ b[n]) * 0x100000001b3ULL;
    return h;
}

static void open_cache(struct thumbnailer *t, struct demuxer *d)
{
    if (!t->cache_dir || !t->cache_dir[0])
        return;
    char *dir = mp_get_user_path(NULL, t->global, t->cache_dir);
    mp_mkdirp(dir);

    struct cache_header hdr = {
        .w = t->w, .h = t->h, .count = t->count, .step = t->step,
    };
    memcpy(hdr.magic, magic, sizeof(hdr.magic));
    uint64_t key = 0xcbf29ce484222325ULL;
    key = fnv(key, t->url, strlen(t->url));
    key = fnv(key, &d->filesize, sizeof(d->filesize));
    key = fnv(key, &d->duration, sizeof(d->duration));
    hdr.key = key;

    char name[40];
    snprintf(name, sizeof(name), "%016llx.thumbs", (unsigned long long)key);
    char *path = mp_path_join(NULL, dir, name);
    talloc_free(dir);

    FILE *f = fopen(path, "r+b");
    struct cache_header old;
    if (f && (fread(&old, sizeof(old), 1, f) != 1 ||
              memcmp(&old, &hdr, sizeof(hdr)) != 0))
    {
        fclose(f);
        f = NULL;
    }
    if (f) {
        fread(t->state, t->count, 1, f);
        for (int i = 0; i < t->count; i++) {
            if (t->state[i] != SLOT_DONE)
                continue;
            uint8_t *buf = talloc_size(t, t->slot_size);
            long off = sizeof(hdr) + t->count + (long)i * t->slot_size;
            if (fseek(f, off, SEEK_SET) || fread(buf, t->slot_size, 1, f) != 1) {
                talloc_free(buf);
                t->state[i] = SLOT_EMPTY;
                continue;
            }
            t->slots[i] = buf;
            t->done++;
        }
        MP_VERBOSE(t, "Loaded %d thumbnails from %s\n", t->done, path);
    } else {
        f = fopen(path, "w+b");
        if (f) {
            fwrite(&hdr, sizeof(hdr), 1, f);
            fwrite(t->state, t->count, 1, f);
        } else {
            MP_WARN(t, "Could not create thumbnail cache %s\n", path);
        }
    }
    for (int i = 0; i < t->count; i++) {
        if (t->state[i] == SLOT_FAILED)
            t->state[i] = SLOT_EMPTY;
    }
    t->cache = f;
    talloc_free(path);
}

static void store_cache(struct thumbnailer *t, int i)
{
    if (!t->cache)
        return;
    long base = sizeof(struct cache_header);
    fseek(t->cache, base + (long)t->count + (long)i * t->slot_size, SEEK_SET);
    fwrite(t->slots[i], t->slot_size, 1, t->cache);
    fseek(t->cache, base + i, SEEK_SET);
    fwrite(&t->state[i], 1, 1, t->cache);
    fflush(t->cache);
}

static int next_index(struct thumbnailer *t)
{
    if (t->want >= 0 && t->state[t->want] == SLOT_EMPTY)
        return t->want;
    if (t->hold || !t->eager)
        return -1;
    if (t->hover >= 0) {
        for (int d = 1; d <= 8; d++) {
            int i = t->hover + t->dir * d;
            if (t->dir && i >= 0 && i < t->count && t->state[i] == SLOT_EMPTY)
                return i;
        }
        for (int d = 1; d <= 16; d++) {
            if (t->hover - d >= 0 && t->state[t->hover - d] == SLOT_EMPTY)
                return t->hover - d;
            if (t->hover + d < t->count && t->state[t->hover + d] == SLOT_EMPTY)
                return t->hover + d;
        }
    }
    if (!t->sweep) {
        for (int i = 0; t->hover >= 0 && i < t->count; i += 16) {
            if (t->state[i] == SLOT_EMPTY)
                return i;
        }
        return -1;
    }
    for (int stride = 8; stride >= 1; stride /= 2) {
        for (int i = 0; i < t->count; i += stride) {
            if (t->state[i] == SLOT_EMPTY)
                return i;
        }
    }
    return -1;
}

static enum AVHWDeviceType auto_hwdec(void)
{
#if defined(__APPLE__)
    return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#elif defined(_WIN32)
    return AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(__linux__) && !defined(__ANDROID__)
    return AV_HWDEVICE_TYPE_VAAPI;
#else
    return AV_HWDEVICE_TYPE_NONE;
#endif
}

static AVCodecContext *open_decoder(struct thumbnailer *t, struct sh_stream *sh,
                                    int hwdec)
{
    enum AVCodecID id = mp_codec_to_av_codec_id(sh->codec->codec);
    const AVCodec *codec = NULL;
    if (hwdec == 2) {
        const AVCodec *sw = avcodec_find_decoder(id);
        if (sw)
            codec = avcodec_find_decoder_by_name(mp_tprintf(64, "%s_mediacodec", sw->name));
    } else {
        codec = avcodec_find_decoder(id);
    }
    if (!codec)
        return NULL;
    AVCodecContext *avctx = avcodec_alloc_context3(codec);
    if (!avctx)
        return NULL;
    if (mp_set_avctx_codec_headers(avctx, sh->codec) < 0)
        goto fail;
    avctx->pkt_timebase = mp_get_codec_timebase(sh->codec);
    avctx->width = sh->codec->disp_w;
    avctx->height = sh->codec->disp_h;
    avctx->skip_frame = AVDISCARD_NONKEY;
    avctx->skip_loop_filter = AVDISCARD_ALL;
    avctx->flags2 |= AV_CODEC_FLAG2_FAST;
    avctx->thread_count = 1;
    if (hwdec == 1) {
        enum AVHWDeviceType type = auto_hwdec();
        if (type == AV_HWDEVICE_TYPE_NONE ||
            av_hwdevice_ctx_create(&avctx->hw_device_ctx, type, NULL, NULL, 0) < 0)
            goto fail;
    } else if (!hwdec) {
        int lowres = 0;
        while (lowres < codec->max_lowres && (sh->codec->disp_w >> (lowres + 1)) >= t->w * 2)
            lowres++;
        avctx->lowres = lowres;
    }
    if (avcodec_open2(avctx, codec, NULL) < 0)
        goto fail;
    return avctx;
fail:
    avcodec_free_context(&avctx);
    return NULL;
}

static uint8_t *scale_frame(struct thumbnailer *t, struct mp_sws_context *sws,
                            AVFrame *frame)
{
    struct mp_image *src = mp_image_from_av_frame(frame);
    if (!src)
        return NULL;
    struct mp_image *dst = mp_image_alloc(IMGFMT_420P, t->w, t->h);
    uint8_t *buf = NULL;
    if (dst && mp_sws_scale(sws, dst, src) >= 0) {
        buf = talloc_size(t, t->slot_size);
        uint8_t *p = buf;
        for (int n = 0; n < 3; n++) {
            int pw = n ? t->w / 2 : t->w, ph = n ? t->h / 2 : t->h;
            for (int y = 0; y < ph; y++) {
                memcpy(p, dst->planes[n] + y * dst->stride[n], pw);
                p += pw;
            }
        }
    }
    talloc_free(src);
    talloc_free(dst);
    return buf;
}

static uint8_t *grab(struct thumbnailer *t, struct demuxer *d,
                     struct sh_stream *sh, AVCodecContext *avctx,
                     struct mp_sws_context *sws, AVPacket *pkt, AVFrame *frame,
                     double pts)
{
    demux_seek(d, pts, 0);
    avcodec_flush_buffers(avctx);
    AVRational tb = mp_get_codec_timebase(sh->codec);
    bool sent = false;
    for (int n = 0; n < 500 && !mp_cancel_test(t->cancel); n++) {
        struct demux_packet *dp = demux_read_any_packet(d);
        if (!dp)
            break;
        if (dp->stream != sh->index || (!sent && !dp->keyframe)) {
            talloc_free(dp);
            continue;
        }
        mp_set_av_packet(pkt, dp, &tb);
        int r = avcodec_send_packet(avctx, pkt);
        talloc_free(dp);
        if (r < 0 && r != AVERROR(EAGAIN))
            continue;
        sent = true;
        break;
    }
    if (!sent)
        return NULL;
    if (avcodec_receive_frame(avctx, frame) >= 0)
        goto got;
    avcodec_send_packet(avctx, NULL);
    int r;
    for (int n = 0; n < 200; n++) {
        r = avcodec_receive_frame(avctx, frame);
        if (r != AVERROR(EAGAIN) || mp_cancel_test(t->cancel))
            break;
        mp_sleep_ns(MP_TIME_MS_TO_NS(5));
    }
    if (r < 0)
        return NULL;
got:;
    AVFrame *src = frame;
    if (frame->hw_frames_ctx) {
        src = av_frame_alloc();
        if (!src || av_hwframe_transfer_data(src, frame, 0) < 0) {
            av_frame_free(&src);
            av_frame_unref(frame);
            return NULL;
        }
    }
    uint8_t *buf = scale_frame(t, sws, src);
    if (src != frame)
        av_frame_free(&src);
    av_frame_unref(frame);
    return buf;
}

static struct sh_stream *pick_video(struct demuxer *d, int width)
{
    struct sh_stream *best = NULL;
    for (int n = 0; n < demux_get_num_stream(d); n++) {
        struct sh_stream *sh = demux_get_stream(d, n);
        if (sh->type != STREAM_VIDEO || sh->attached_picture ||
            sh->codec->disp_w <= 0 || sh->codec->disp_h <= 0)
            continue;
        if (!best) {
            best = sh;
            continue;
        }
        if (sh->hls_bitrate <= 0 || best->hls_bitrate <= 0)
            continue;
        bool fits = sh->codec->disp_w >= width;
        bool best_fits = best->codec->disp_w >= width;
        if ((fits && !best_fits) ||
            (fits == best_fits && fits && sh->hls_bitrate < best->hls_bitrate) ||
            (!fits && !best_fits && sh->hls_bitrate > best->hls_bitrate))
            best = sh;
    }
    return best;
}

static void setup(struct thumbnailer *t, struct demuxer *d, struct sh_stream *sh)
{
    struct mp_codec_params *c = sh->codec;
    double aspect = (double)c->disp_w / c->disp_h;
    if (c->par_w > 0 && c->par_h > 0)
        aspect = aspect * c->par_w / c->par_h;
    t->w = MPMAX(t->width & ~1, 16);
    t->h = MPMAX((int)lrint(t->w / aspect) & ~1, 16);
    t->step = MPMAX(t->interval, d->duration / t->max_count);
    t->count = MPMAX((int)ceil(d->duration / t->step), 1);
    t->slot_size = t->w * t->h * 3 / 2;
    t->state = talloc_zero_array(t, uint8_t, t->count);
    t->slots = talloc_zero_array(t, uint8_t *, t->count);
}

static void lower_priority(void)
{
#ifdef __linux__
    setpriority(PRIO_PROCESS, 0, 10);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

static MP_THREAD_VOID thumbnail_thread(void *arg)
{
    struct thumbnailer *t = arg;
    mp_thread_set_name("thumbnail");
    lower_priority();

    mp_stream_lavf_set_thread_headers(t->headers);
    struct demuxer_params params = {.stream_flags = t->stream_flags | STREAM_SPARSE_READS};
    struct demuxer *d = demux_open_url(t->url, &params, t->cancel, t->global);
    if (!d) {
        MP_WARN(t, "Could not open %s\n", t->url);
        goto out;
    }
    if (t->rebase)
        demux_set_ts_offset(d, -d->start_time);
    struct sh_stream *sh = pick_video(d, t->width);
    if (!sh || !d->seekable || !(d->duration > 0)) {
        MP_VERBOSE(t, "Nothing to thumbnail.\n");
        goto out;
    }
    demuxer_select_track(d, sh, MP_NOPTS_VALUE, true);

    AVCodecContext *avctx = NULL;
    struct mp_sws_context *sws = mp_sws_alloc(NULL);
    sws->flags = mp_sws_fast_flags;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    setup(t, d, sh);
    open_cache(t, d);

    int hwdec = t->hwdec;
    avctx = hwdec ? open_decoder(t, sh, hwdec) : NULL;
    if (!avctx && hwdec) {
        MP_VERBOSE(t, "Hardware decoding unavailable, using software.\n");
        hwdec = 0;
    }
    if (!avctx)
        avctx = open_decoder(t, sh, 0);
    bool hw_ok = false;
    if (!avctx) {
        MP_WARN(t, "Could not open decoder for %s\n", sh->codec->codec);
        goto done;
    }

    double start = mp_time_sec();
    mp_mutex_lock(&t->lock);
    t->ready = true;
    t->changed = true;
    MP_VERBOSE(t, "%dx%d, %d thumbnails every %.1fs\n", t->w, t->h, t->count, t->step);
    t->wakeup(t->wakeup_ctx);
    double last_wakeup = mp_time_sec();
    int64_t eager_at = mp_time_ns() + MP_TIME_S_TO_NS(t->delay);
    bool pending = false;
    bool primed = false;
    while (!t->quit) {
        int i = next_index(t);
        if (i < 0) {
            if (pending) {
                t->wakeup(t->wakeup_ctx);
                pending = false;
            }
            if (t->done == t->count) {
                MP_VERBOSE(t, "Done in %.2fs\n", mp_time_sec() - start);
                break;
            }
            if (t->eager) {
                mp_cond_wait(&t->cond, &t->lock);
            } else if (mp_cond_timedwait_until(&t->cond, &t->lock, eager_at)) {
                t->eager = true;
            }
            continue;
        }
        if (!primed && t->state[0] != SLOT_EMPTY) {
            mp_mutex_unlock(&t->lock);
            talloc_free(grab(t, d, sh, avctx, sws, pkt, frame, 0));
            mp_mutex_lock(&t->lock);
            primed = true;
            continue;
        }
        if (!primed)
            i = 0;
        primed = true;
        mp_mutex_unlock(&t->lock);
        uint8_t *buf = grab(t, d, sh, avctx, sws, pkt, frame, i * t->step);
        if (!buf && hwdec && !hw_ok && !mp_cancel_test(t->cancel)) {
            AVCodecContext *sw = open_decoder(t, sh, 0);
            uint8_t *sw_buf = NULL;
            if (sw) {
                if (i)
                    talloc_free(grab(t, d, sh, sw, sws, pkt, frame, 0));
                sw_buf = grab(t, d, sh, sw, sws, pkt, frame, i * t->step);
            }
            if (sw_buf) {
                MP_VERBOSE(t, "Hardware decoding failed, using software.\n");
                avcodec_free_context(&avctx);
                avctx = sw;
                hwdec = 0;
                buf = sw_buf;
            } else {
                avcodec_free_context(&sw);
            }
        }
        hw_ok |= !!buf;
        mp_mutex_lock(&t->lock);
        if (mp_cancel_test(t->cancel)) {
            talloc_free(buf);
            break;
        }
        t->state[i] = buf ? SLOT_DONE : SLOT_FAILED;
        t->slots[i] = buf;
        bool wanted = t->want == i;
        if (wanted)
            t->want = -1;
        t->done++;
        t->changed = true;
        double now = mp_time_sec();
        if (wanted || now - last_wakeup >= 0.1) {
            t->wakeup(t->wakeup_ctx);
            last_wakeup = now;
            pending = false;
        } else {
            pending = true;
        }
        if (buf) {
            mp_mutex_unlock(&t->lock);
            store_cache(t, i);
            mp_mutex_lock(&t->lock);
        }
    }
    mp_mutex_unlock(&t->lock);

done:
    avcodec_free_context(&avctx);
    mp_free_av_packet(&pkt);
    av_frame_free(&frame);
    talloc_free(sws);
out:
    demux_free(d);
    mp_mutex_lock(&t->lock);
    t->finished = true;
    mp_mutex_unlock(&t->lock);
    t->wakeup(t->wakeup_ctx);
    MP_THREAD_RETURN();
}

void mp_thumbnails_start(struct MPContext *mpctx)
{
    mp_thumbnails_stop(mpctx);
    struct MPOpts *opts = mpctx->opts;
    if (!opts->thumbnails || !mpctx->stream_open_filename || !mpctx->vo_chain ||
        mpctx->vo_chain->is_coverart || !mpctx->demuxer || !mpctx->demuxer->seekable)
        return;

    struct thumbnailer *t = talloc_zero(NULL, struct thumbnailer);
    t->global = mpctx->global;
    t->log = mp_log_new(t, mpctx->log, "thumbnail");
    t->url = talloc_strdup(t, opts->thumbnail_url && opts->thumbnail_url[0]
                              ? opts->thumbnail_url : mpctx->stream_open_filename);
    t->stream_flags = mpctx->playing ? mpctx->playing->stream_flags : 0;
    t->rebase = opts->rebase_start_time;
    t->width = opts->thumbnail_width;
    t->interval = opts->thumbnail_interval;
    t->max_count = opts->thumbnail_max;
    t->sweep = opts->thumbnail_sweep;
    t->cache_dir = talloc_strdup(t, opts->thumbnail_cache_dir);
    t->delay = opts->thumbnail_delay;
    t->eager = t->delay <= 0;
    t->headers = NULL;
    for (int n = 0; opts->thumbnail_headers && opts->thumbnail_headers[n]; n++) {
        t->headers = talloc_asprintf_append(t->headers, "%s\r\n",
                                            opts->thumbnail_headers[n]);
    }
    if (t->headers)
        talloc_steal(t, t->headers);
    t->hwdec = opts->thumbnail_hwdec;
    t->wakeup = mp_wakeup_core_cb;
    t->wakeup_ctx = mpctx;
    t->cancel = mp_cancel_new(t);
    t->want = -1;
    t->hover = -1;
    t->out_sws = mp_sws_alloc(t);
    t->out_sws->flags = mp_sws_fast_flags;
    mp_mutex_init(&t->lock);
    mp_cond_init(&t->cond);
    if (mp_thread_create(&t->thread, thumbnail_thread, t)) {
        mp_mutex_destroy(&t->lock);
        mp_cond_destroy(&t->cond);
        talloc_free(t);
        return;
    }
    mpctx->thumbnailer = t;
    mp_notify_property(mpctx, "thumbnail-info");
}

static void reap(struct MPContext *mpctx, bool wait)
{
    for (int n = mpctx->num_old_thumbnailers - 1; n >= 0; n--) {
        struct thumbnailer *t = mpctx->old_thumbnailers[n];
        mp_mutex_lock(&t->lock);
        bool finished = t->finished;
        mp_mutex_unlock(&t->lock);
        if (!finished && !wait)
            continue;
        mp_thread_join(t->thread);
        if (t->cache)
            fclose(t->cache);
        mp_mutex_destroy(&t->lock);
        mp_cond_destroy(&t->cond);
        talloc_free(t);
        MP_TARRAY_REMOVE_AT(mpctx->old_thumbnailers, mpctx->num_old_thumbnailers, n);
    }
}

void mp_thumbnails_stop(struct MPContext *mpctx)
{
    struct thumbnailer *t = mpctx->thumbnailer;
    if (!t)
        return;
    mp_mutex_lock(&t->lock);
    t->quit = true;
    mp_cond_signal(&t->cond);
    mp_mutex_unlock(&t->lock);
    mp_cancel_trigger(t->cancel);
    MP_TARRAY_APPEND(mpctx, mpctx->old_thumbnailers, mpctx->num_old_thumbnailers, t);
    mpctx->thumbnailer = NULL;
    mp_notify_property(mpctx, "thumbnail-info");
}

void mp_thumbnails_uninit(struct MPContext *mpctx)
{
    mp_thumbnails_stop(mpctx);
    reap(mpctx, true);
}

void mp_thumbnails_update(struct MPContext *mpctx)
{
    reap(mpctx, false);
    struct thumbnailer *t = mpctx->thumbnailer;
    if (!t)
        return;
    mp_mutex_lock(&t->lock);
    bool hold = mpctx->paused_for_cache;
    if (hold != t->hold) {
        t->hold = hold;
        mp_cond_signal(&t->cond);
    }
    bool changed = t->changed;
    t->changed = false;
    mp_mutex_unlock(&t->lock);
    if (changed)
        mp_notify_property(mpctx, "thumbnail-info");
}

bool mp_thumbnails_info(struct MPContext *mpctx, struct mpv_node *res)
{
    struct thumbnailer *t = mpctx->thumbnailer;
    if (!t)
        return false;
    mp_mutex_lock(&t->lock);
    bool ready = t->ready;
    if (ready) {
        node_init(res, MPV_FORMAT_NODE_MAP, NULL);
        node_map_add_int64(res, "w", t->w);
        node_map_add_int64(res, "h", t->h);
        node_map_add_int64(res, "count", t->count);
        node_map_add_int64(res, "done", t->done);
        node_map_add_double(res, "interval", t->step);
    }
    mp_mutex_unlock(&t->lock);
    return ready;
}

void cmd_thumbnail(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    struct thumbnailer *t = mpctx->thumbnailer;
    cmd->success = false;
    if (!t)
        return;

    mp_mutex_lock(&t->lock);
    if (!t->ready) {
        mp_mutex_unlock(&t->lock);
        return;
    }
    int want = MPCLAMP((int)lrint(cmd->args[0].v.d / t->step), 0, t->count - 1);
    if (want != t->hover)
        t->dir = want > t->hover ? 1 : -1;
    t->hover = want;
    if (!t->eager) {
        t->eager = true;
        mp_cond_signal(&t->cond);
    }
    if (t->state[want] == SLOT_EMPTY && t->want != want) {
        t->want = want;
        mp_cond_signal(&t->cond);
    }
    int best = -1;
    for (int d = 0; d < t->count && best < 0; d++) {
        if (want - d >= 0 && t->slots[want - d])
            best = want - d;
        else if (want + d < t->count && t->slots[want + d])
            best = want + d;
    }
    if (best < 0) {
        mp_mutex_unlock(&t->lock);
        return;
    }

    struct mp_image src = {0};
    mp_image_setfmt(&src, IMGFMT_420P);
    mp_image_set_size(&src, t->w, t->h);
    uint8_t *buf = t->slots[best];
    src.planes[0] = buf;
    src.stride[0] = t->w;
    src.planes[1] = buf + t->w * t->h;
    src.stride[1] = t->w / 2;
    src.planes[2] = src.planes[1] + t->w / 2 * t->h / 2;
    src.stride[2] = t->w / 2;
    mp_image_params_guess_csp(&src.params);

    struct mp_image *img = mp_image_alloc(IMGFMT_BGRA, t->w, t->h);
    bool ok = img && mp_sws_scale(t->out_sws, img, &src) >= 0;
    double pts = best * t->step;
    mp_mutex_unlock(&t->lock);
    if (!ok) {
        talloc_free(img);
        return;
    }

    int id = cmd->args[1].v.i;
    struct mpv_node *res = &cmd->result;
    node_init(res, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_int64(res, "w", img->w);
    node_map_add_int64(res, "h", img->h);
    node_map_add_int64(res, "stride", img->stride[0]);
    node_map_add_string(res, "format", "bgra");
    node_map_add_double(res, "time", pts);
    node_map_add_flag(res, "exact", best == want);
    if (id >= 0 && id < 64) {
        int dw = cmd->args[4].v.i > 0 ? cmd->args[4].v.i : img->w;
        int dh = cmd->args[5].v.i > 0 ? cmd->args[5].v.i : img->h;
        mp_set_overlay(mpctx, id, img, cmd->args[2].v.i, cmd->args[3].v.i, dw, dh);
        cmd->success = true;
        return;
    }
    struct mpv_byte_array *ba = node_map_add(res, "data", MPV_FORMAT_BYTE_ARRAY)->u.ba;
    *ba = (struct mpv_byte_array){
        .data = img->planes[0],
        .size = img->stride[0] * img->h,
    };
    talloc_steal(ba, img);
    cmd->success = true;
}

void cmd_thumbnail_reload(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    mp_thumbnails_start(cmd->mpctx);
}
