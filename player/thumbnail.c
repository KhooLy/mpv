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

#ifndef _WIN32
#include <unistd.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/cpu.h>
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

enum { SLOT_EMPTY, SLOT_DONE, SLOT_FAILED, SLOT_BUSY };

#define HLS_HEAD (128 * 1024)

struct hls_seg {
    double start;
    char *url;
};

struct membuf {
    const uint8_t *data;
    size_t size, pos;
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
    int resident;
    int cap;
    int64_t memory;

    size_t slot_size;
    struct mp_sws_context *out_sws;
    int workers;
    mp_thread *threads;
    int nthreads;
    bool auto_workers;
    struct hls_seg *segs;
    int nsegs;
    uint8_t *init;
    size_t init_size;
    AVDictionary *net;
    int strides[3];
    int nstrides;
    int pass;
    struct sh_stream *sh;
    double hls_duration;
    bool direct;
    int vindex;
    int *kfidx;
};

static int next_index(struct thumbnailer *t)
{
    if (t->want >= 0 && t->state[t->want] == SLOT_EMPTY)
        return t->want;
    if (t->hold || !t->eager || t->resident >= t->cap)
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
    for (int n = 0; n < t->nstrides; n++) {
        for (int i = 0; i < t->count; i += t->strides[n]) {
            if (t->state[i] != SLOT_EMPTY)
                continue;
            if (n > t->pass) {
                t->pass = n;
                MP_VERBOSE(t, "Refining to every %.1fs, %d/%d done\n",
                           t->strides[n] * t->step, t->done, t->count);
            }
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

static int interrupted(void *ctx)
{
    return mp_cancel_test(ctx);
}

static uint8_t *fetch(struct thumbnailer *t, const char *url, int64_t limit, size_t *size)
{
    AVDictionary *opts = NULL;
    av_dict_copy(&opts, t->net, 0);
    if (t->headers)
        av_dict_set(&opts, "headers", t->headers, 0);
    if (limit)
        av_dict_set_int(&opts, "end_offset", limit, 0);
    av_dict_set_int(&opts, "rw_timeout", 15000000, 0);
    AVIOInterruptCB cb = {interrupted, t->cancel};
    AVIOContext *io = NULL;
    int r = avio_open2(&io, url, AVIO_FLAG_READ, &cb, &opts);
    av_dict_free(&opts);
    if (r < 0)
        return NULL;
    size_t cap = limit ? limit : 1024 * 1024, n = 0;
    uint8_t *buf = av_malloc(cap + 1);
    for (;;) {
        if (n == cap) {
            if (limit)
                break;
            cap *= 2;
            buf = av_realloc(buf, cap + 1);
        }
        r = avio_read(io, buf + n, cap - n);
        if (r <= 0)
            break;
        n += r;
    }
    avio_closep(&io);
    if (!n) {
        av_free(buf);
        return NULL;
    }
    buf[n] = 0;
    *size = n;
    return buf;
}

static char *hls_resolve(struct thumbnailer *t, const char *base, const char *ref)
{
    if (!strncmp(ref, "http://", 7) || !strncmp(ref, "https://", 8))
        return talloc_strdup(t, ref);
    const char *host = strstr(base, "://");
    if (!host)
        return NULL;
    host += 3;
    if (ref[0] == '/') {
        const char *end = strchr(host, '/');
        return talloc_asprintf(t, "%.*s%s", (int)(end ? end - base : strlen(base)), base, ref);
    }
    size_t len = strcspn(base, "?#");
    while (len > (size_t)(host - base) && base[len - 1] != '/')
        len--;
    return talloc_asprintf(t, "%.*s%s", (int)len, base, ref);
}

static char *hls_variant(struct thumbnailer *t, char *pl, const char *base)
{
    char *best = NULL;
    int best_bw = 0, best_w = 0, bw = 0, w = 0;
    bool pending = false;
    for (char *line = pl, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r')
            line[len - 1] = 0;
        if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
            char *b = strstr(line, "BANDWIDTH=");
            char *r = strstr(line, "RESOLUTION=");
            bw = b ? atoi(b + 10) : 0;
            w = r ? atoi(r + 11) : 0;
            pending = true;
        } else if (pending && *line && *line != '#') {
            pending = false;
            bool fits = w >= t->width;
            bool best_fits = best_w >= t->width;
            if (!best || (fits && !best_fits) ||
                (fits == best_fits && fits && bw < best_bw) ||
                (!fits && !best_fits && bw > best_bw))
            {
                char *url = hls_resolve(t, base, line);
                if (!url)
                    continue;
                talloc_free(best);
                best = url;
                best_bw = bw;
                best_w = w;
            }
        }
    }
    return best;
}

static void net_setup(struct thumbnailer *t)
{
    if (strncmp(t->url, "http", 4))
        return;
    mp_setup_av_network_options(&t->net, NULL, t->global, t->log);
    av_dict_set(&t->net, "reconnect", NULL, 0);
    av_dict_set(&t->net, "icy", NULL, 0);
}

static bool hls_sniff(struct thumbnailer *t)
{
    if (strncmp(t->url, "http", 4))
        return false;
    size_t n = 0;
    uint8_t *head = fetch(t, t->url, 7, &n);
    bool hls = head && n == 7 && !memcmp(head, "#EXTM3U", 7);
    av_free(head);
    return hls;
}

static bool hls_load(struct thumbnailer *t)
{
    if (!hls_sniff(t))
        return false;
    size_t n;
    const char *base = t->url;
    char *pl = NULL;
    char *map = NULL;
    for (int depth = 0; depth < 2; depth++) {
        pl = (char *)fetch(t, base, 0, &n);
        if (!pl || strncmp(pl, "#EXTM3U", 7))
            goto fail;
        if (!strstr(pl, "#EXT-X-STREAM-INF"))
            break;
        base = hls_variant(t, pl, base);
        av_free(pl);
        pl = NULL;
        if (!base)
            goto fail;
    }
    if (!pl)
        goto fail;
    double dur = 0, at = 0;
    for (char *line = pl, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r')
            line[len - 1] = 0;
        if (!strncmp(line, "#EXTINF:", 8)) {
            dur = atof(line + 8);
        } else if (!strncmp(line, "#EXT-X-MAP:", 11)) {
            char *u = strstr(line, "URI=\"");
            char *e = u ? strchr(u + 5, '"') : NULL;
            if (!e)
                goto fail;
            *e = 0;
            map = hls_resolve(t, base, u + 5);
            if (!map)
                goto fail;
        } else if (!strncmp(line, "#EXT-X-KEY", 10) ||
                   !strncmp(line, "#EXT-X-BYTERANGE", 16)) {
            goto fail;
        } else if (*line && *line != '#') {
            char *url = dur > 0 ? hls_resolve(t, base, line) : NULL;
            if (!url)
                goto fail;
            struct hls_seg seg = {at, url};
            MP_TARRAY_APPEND(t, t->segs, t->nsegs, seg);
            at += dur;
            dur = 0;
        }
    }
    if (!map || !t->nsegs)
        goto fail;
    t->hls_duration = at;
    uint8_t *init = fetch(t, map, 0, &t->init_size);
    if (!init)
        goto fail;
    t->init = init;
    av_free(pl);
    return true;

fail:
    t->nsegs = 0;
    av_free(pl);
    return false;
}

static int mem_read(void *opaque, uint8_t *buf, int size)
{
    struct membuf *m = opaque;
    if (m->pos >= m->size)
        return AVERROR_EOF;
    int c = MPMIN((size_t)size, m->size - m->pos);
    memcpy(buf, m->data + m->pos, c);
    m->pos += c;
    return c;
}

static bool hls_try(struct thumbnailer *t, int i, AVPacket *pkt)
{
    for (int64_t head = HLS_HEAD; head <= 4 * HLS_HEAD; head *= 4) {
        size_t n;
        uint8_t *part = fetch(t, t->segs[i].url, head, &n);
        if (!part)
            part = fetch(t, t->segs[i].url, 0, &n);
        if (!part)
            return false;
        uint8_t *data = av_malloc(t->init_size + n);
        memcpy(data, t->init, t->init_size);
        memcpy(data + t->init_size, part, n);
        av_free(part);
        struct membuf m = {data, t->init_size + n, 0};
        uint8_t *iobuf = av_malloc(32768);
        AVIOContext *io = iobuf ? avio_alloc_context(iobuf, 32768, 0, &m, mem_read, NULL, NULL) : NULL;
        AVFormatContext *fc = io ? avformat_alloc_context() : NULL;
        bool ok = false;
        if (fc) {
            fc->pb = io;
            fc->flags |= AVFMT_FLAG_CUSTOM_IO;
            if (avformat_open_input(&fc, NULL, NULL, NULL) >= 0) {
                while (av_read_frame(fc, pkt) >= 0) {
                    if (pkt->stream_index == 0 && (pkt->flags & AV_PKT_FLAG_KEY) &&
                        !(pkt->flags & AV_PKT_FLAG_CORRUPT))
                    {
                        ok = true;
                        break;
                    }
                    av_packet_unref(pkt);
                }
                avformat_close_input(&fc);
            }
        }
        if (io)
            av_freep(&io->buffer);
        avio_context_free(&io);
        av_free(data);
        if (ok)
            return true;
        if (n < head)
            return false;
    }
    return false;
}

static int hls_index(struct thumbnailer *t, double pts)
{
    int i = 0;
    while (i + 1 < t->nsegs && t->segs[i + 1].start <= pts)
        i++;
    return i;
}

static bool hls_keyframe(struct thumbnailer *t, double pts, AVPacket *pkt)
{
    return hls_try(t, hls_index(t, pts), pkt);
}

static int slot_group(struct thumbnailer *t, int i)
{
    return t->kfidx ? t->kfidx[i] : hls_index(t, i * t->step);
}

static uint8_t *twin_slot(struct thumbnailer *t, int i)
{
    if (!t->nsegs && !t->kfidx)
        return NULL;
    int seg = slot_group(t, i);
    for (int d = 1; d <= 3; d++) {
        for (int j = i - d; j <= i + d; j += 2 * d) {
            if (j >= 0 && j < t->count && t->slots[j] &&
                slot_group(t, j) == seg)
                return talloc_memdup(NULL, t->slots[j], t->slot_size);
        }
    }
    return NULL;
}

static struct sh_stream *sh_from_par(struct thumbnailer *t, AVCodecParameters *par)
{
    if (!par || par->codec_type != AVMEDIA_TYPE_VIDEO || par->width <= 0 ||
        par->height <= 0)
        return NULL;
    struct sh_stream *sh = demux_alloc_sh_stream(STREAM_VIDEO);
    struct mp_codec_params *c = sh->codec;
    c->codec = talloc_strdup(sh, avcodec_get_name(par->codec_id));
    c->disp_w = par->width;
    c->disp_h = par->height;
    if (par->sample_aspect_ratio.num > 0) {
        c->par_w = par->sample_aspect_ratio.num;
        c->par_h = par->sample_aspect_ratio.den;
    }
    if (par->extradata_size > 0) {
        c->extradata = talloc_memdup(sh, par->extradata, par->extradata_size);
        c->extradata_size = par->extradata_size;
    }
    talloc_steal(t, sh);
    return sh;
}

static struct sh_stream *hls_stream(struct thumbnailer *t)
{
    struct membuf m = {t->init, t->init_size, 0};
    uint8_t *iobuf = av_malloc(32768);
    AVIOContext *io = iobuf ? avio_alloc_context(iobuf, 32768, 0, &m, mem_read, NULL, NULL) : NULL;
    AVFormatContext *fc = io ? avformat_alloc_context() : NULL;
    struct sh_stream *sh = NULL;
    if (fc) {
        fc->pb = io;
        fc->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_open_input(&fc, NULL, NULL, NULL) >= 0) {
            sh = sh_from_par(t, fc->nb_streams ? fc->streams[0]->codecpar : NULL);
            avformat_close_input(&fc);
        }
    }
    if (io)
        av_freep(&io->buffer);
    avio_context_free(&io);
    return sh;
}

static AVFormatContext *direct_open(struct thumbnailer *t, int *vindex)
{
    AVDictionary *opts = NULL;
    av_dict_copy(&opts, t->net, 0);
    if (t->headers)
        av_dict_set(&opts, "headers", t->headers, 0);
    av_dict_set_int(&opts, "rw_timeout", 15000000, 0);
    AVFormatContext *fc = avformat_alloc_context();
    if (!fc) {
        av_dict_free(&opts);
        return NULL;
    }
    fc->interrupt_callback = (AVIOInterruptCB){interrupted, t->cancel};
    int r = avformat_open_input(&fc, t->url, NULL, &opts);
    av_dict_free(&opts);
    if (r < 0)
        return NULL;
    int idx = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    AVStream *st = idx >= 0 ? fc->streams[idx] : NULL;
    const AVIndexEntry *e = st ? avformat_index_get_entry(st, 0) : NULL;
    if (!e || e->size <= 0 || avformat_index_get_entries_count(st) < 2 ||
        (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
        st->codecpar->width <= 0 || st->codecpar->height <= 0 ||
        fc->duration <= 0)
    {
        avformat_close_input(&fc);
        return NULL;
    }
    *vindex = idx;
    return fc;
}

static int direct_entry(AVStream *st, double pts)
{
    int64_t ts = av_rescale_q(lrint(pts * AV_TIME_BASE), AV_TIME_BASE_Q, st->time_base);
    if (st->start_time != AV_NOPTS_VALUE)
        ts += st->start_time;
    int i = av_index_search_timestamp(st, ts, AVSEEK_FLAG_BACKWARD);
    return i >= 0 ? i : av_index_search_timestamp(st, ts, 0);
}

static bool direct_keyframe(struct thumbnailer *t, AVFormatContext *fc, double pts,
                            AVPacket *pkt)
{
    AVStream *st = fc->streams[t->vindex];
    int i = direct_entry(st, pts);
    const AVIndexEntry *e = i >= 0 ? avformat_index_get_entry(st, i) : NULL;
    if (!e || e->size <= 0 || e->size > 64 * 1024 * 1024 ||
        av_new_packet(pkt, e->size) < 0)
        return false;
    if (avio_seek(fc->pb, e->pos, SEEK_SET) < 0 ||
        avio_read(fc->pb, pkt->data, e->size) != e->size)
    {
        av_packet_unref(pkt);
        return false;
    }
    pkt->flags |= AV_PKT_FLAG_KEY;
    return true;
}

static uint8_t *grab(struct thumbnailer *t, struct demuxer *d, AVFormatContext *fc,
                     struct sh_stream *sh, AVCodecContext *avctx,
                     struct mp_sws_context *sws, AVPacket *pkt, AVFrame *frame,
                     double pts)
{
    avcodec_flush_buffers(avctx);
    bool sent = false;
    if (t->nsegs) {
        AVPacket *hp = av_packet_alloc();
        if (hp && hls_keyframe(t, pts, hp))
            sent = avcodec_send_packet(avctx, hp) >= 0;
        av_packet_free(&hp);
    } else if (fc) {
        AVPacket *hp = av_packet_alloc();
        if (hp && direct_keyframe(t, fc, pts, hp))
            sent = avcodec_send_packet(avctx, hp) >= 0;
        av_packet_free(&hp);
    }
    if (!sent && d)
        demux_seek(d, pts, 0);
    AVRational tb = mp_get_codec_timebase(sh->codec);
    for (int n = 0; !sent && d && n < 500 && !mp_cancel_test(t->cancel); n++) {
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

static int64_t ram_bytes(void)
{
#ifdef _WIN32
    MEMORYSTATUSEX status = {.dwLength = sizeof(status)};
    return GlobalMemoryStatusEx(&status) ? (int64_t)status.ullTotalPhys : 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    return pages > 0 && page_size > 0 ? (int64_t)pages * page_size : 0;
#endif
}

static int64_t slot_budget(void)
{
    int64_t ram = ram_bytes();
    if (ram <= 0)
        return 64 * 1024 * 1024;
    return MPCLAMP(ram / 32, 32 * 1024 * 1024, 128 * 1024 * 1024);
}

static int auto_workers(const char *url, int nsegs)
{
    int cpus = av_cpu_count();
    bool remote = !strncmp(url, "http://", 7) || !strncmp(url, "https://", 8);
    if (remote)
        return nsegs ? MPCLAMP(cpus * 2, 4, 12) : MPCLAMP(cpus, 4, 8);
    return MPCLAMP(cpus / 4, 1, 4);
}

static void setup(struct thumbnailer *t, double duration, struct sh_stream *sh)
{
    struct mp_codec_params *c = sh->codec;
    double aspect = (double)c->disp_w / c->disp_h;
    if (c->par_w > 0 && c->par_h > 0)
        aspect = aspect * c->par_w / c->par_h;
    t->w = MPMAX(t->width & ~1, 16);
    t->h = MPMAX((int)lrint(t->w / aspect) & ~1, 16);
    t->slot_size = t->w * t->h * 3 / 2;
    t->step = MPMAX(t->interval, duration / t->max_count);
    t->count = MPMAX((int)ceil(duration / t->step), 1);
    int64_t budget = t->memory > 0 ? t->memory : slot_budget();
    t->cap = MPMIN(t->count, MPMAX((int)(budget / t->slot_size), 16));
    static const double passes[] = {30, 15};
    t->nstrides = 0;
    for (int n = 0; n < 2; n++) {
        int stride = lrint(passes[n] / t->step);
        if (stride > 1 && (!t->nstrides || stride < t->strides[t->nstrides - 1]))
            t->strides[t->nstrides++] = stride;
    }
    t->strides[t->nstrides++] = 1;
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

struct worker {
    struct demuxer *d;
    AVFormatContext *fc;
    struct sh_stream *sh;
    AVCodecContext *avctx;
    struct mp_sws_context *sws;
    AVPacket *pkt;
    AVFrame *frame;
    int hwdec;
    bool hw_ok;
};

static struct demuxer *open_demux(struct thumbnailer *t, struct sh_stream **sh_out)
{
    struct demuxer_params params = {.stream_flags = t->stream_flags | STREAM_SPARSE_READS};
    struct demuxer *d = demux_open_url(t->url, &params, t->cancel, t->global);
    if (!d)
        return NULL;
    if (t->rebase)
        demux_set_ts_offset(d, -d->start_time);
    struct sh_stream *sh = pick_video(d, t->width);
    if (!sh || !d->seekable || !(d->duration > 0)) {
        demux_free(d);
        return NULL;
    }
    demuxer_select_track(d, sh, MP_NOPTS_VALUE, true);
    *sh_out = sh;
    return d;
}

static bool worker_open(struct thumbnailer *t, struct worker *w)
{
    w->sws = mp_sws_alloc(NULL);
    w->sws->flags = mp_sws_fast_flags;
    w->pkt = av_packet_alloc();
    w->frame = av_frame_alloc();
    w->hwdec = t->hwdec;
    w->avctx = w->hwdec ? open_decoder(t, w->sh, w->hwdec) : NULL;
    if (!w->avctx && w->hwdec) {
        MP_VERBOSE(t, "Hardware decoding unavailable, using software.\n");
        w->hwdec = 0;
    }
    if (!w->avctx)
        w->avctx = open_decoder(t, w->sh, 0);
    return w->avctx != NULL;
}

static void worker_close(struct worker *w)
{
    avcodec_free_context(&w->avctx);
    mp_free_av_packet(&w->pkt);
    av_frame_free(&w->frame);
    talloc_free(w->sws);
    demux_free(w->d);
    avformat_close_input(&w->fc);
}

static int distance(int a, int b)
{
    return a > b ? a - b : b - a;
}

static void evict(struct thumbnailer *t, int keep)
{
    int ref = t->hover >= 0 ? t->hover : keep;
    int victim = -1;
    for (int j = 0; j < t->count; j++) {
        if (j == keep || !t->slots[j])
            continue;
        if (victim < 0 || distance(j, ref) > distance(victim, ref))
            victim = j;
    }
    if (victim < 0)
        return;
    talloc_free(t->slots[victim]);
    t->slots[victim] = NULL;
    t->state[victim] = SLOT_EMPTY;
    t->resident--;
    t->done--;
}

static void work(struct thumbnailer *t, struct worker *w)
{
    int64_t eager_at = mp_time_ns() + MP_TIME_S_TO_NS(t->delay);
    double last_wakeup = mp_time_sec();
    bool pending = false;
    mp_mutex_lock(&t->lock);
    while (!t->quit) {
        int i = next_index(t);
        if (i < 0) {
            if (pending) {
                t->wakeup(t->wakeup_ctx);
                pending = false;
            }
            if (t->done == t->count) {
                mp_cond_broadcast(&t->cond);
                break;
            }
            if (t->eager) {
                mp_cond_wait(&t->cond, &t->lock);
            } else if (mp_cond_timedwait_until(&t->cond, &t->lock, eager_at)) {
                t->eager = true;
            }
            continue;
        }
        t->state[i] = SLOT_BUSY;
        uint8_t *buf = twin_slot(t, i);
        mp_mutex_unlock(&t->lock);
        if (!buf)
            buf = grab(t, w->d, w->fc, w->sh, w->avctx, w->sws, w->pkt, w->frame, i * t->step);
        if (!buf && w->hwdec && !w->hw_ok && !mp_cancel_test(t->cancel)) {
            AVCodecContext *sw = open_decoder(t, w->sh, 0);
            uint8_t *sw_buf = NULL;
            if (sw) {
                if (i)
                    talloc_free(grab(t, w->d, w->fc, w->sh, sw, w->sws, w->pkt, w->frame, 0));
                sw_buf = grab(t, w->d, w->fc, w->sh, sw, w->sws, w->pkt, w->frame, i * t->step);
            }
            if (sw_buf) {
                MP_VERBOSE(t, "Hardware decoding failed, using software.\n");
                avcodec_free_context(&w->avctx);
                w->avctx = sw;
                w->hwdec = 0;
                buf = sw_buf;
            } else {
                avcodec_free_context(&sw);
            }
        }
        w->hw_ok |= !!buf;
        mp_mutex_lock(&t->lock);
        if (mp_cancel_test(t->cancel)) {
            talloc_free(buf);
            break;
        }
        t->state[i] = buf ? SLOT_DONE : SLOT_FAILED;
        t->slots[i] = buf;
        if (buf) {
            t->resident++;
            while (t->resident > t->cap)
                evict(t, i);
        }
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
    }
    mp_mutex_unlock(&t->lock);
}

static MP_THREAD_VOID worker_thread(void *arg)
{
    struct thumbnailer *t = arg;
    mp_thread_set_name("thumbnail");
    lower_priority();

    mp_stream_lavf_set_thread_headers(t->headers);
    struct worker w = {0};
    if (t->nsegs) {
        w.sh = t->sh;
    } else if (t->direct) {
        w.sh = t->sh;
        w.fc = direct_open(t, &t->vindex);
        if (!w.fc)
            MP_THREAD_RETURN();
    } else {
        w.d = open_demux(t, &w.sh);
        if (!w.d)
            MP_THREAD_RETURN();
    }
    if (worker_open(t, &w))
        work(t, &w);
    worker_close(&w);
    MP_THREAD_RETURN();
}

static MP_THREAD_VOID thumbnail_thread(void *arg)
{
    struct thumbnailer *t = arg;
    mp_thread_set_name("thumbnail");
    lower_priority();

    mp_stream_lavf_set_thread_headers(t->headers);
    net_setup(t);
    hls_load(t);
    struct worker w = {0};
    double duration = t->hls_duration;
    if (t->nsegs) {
        w.sh = hls_stream(t);
        if (!w.sh)
            t->nsegs = 0;
    }
    if (!w.sh) {
        w.fc = direct_open(t, &t->vindex);
        if (w.fc) {
            w.sh = sh_from_par(t, w.fc->streams[t->vindex]->codecpar);
            duration = w.fc->duration / (double)AV_TIME_BASE;
            t->direct = !!w.sh;
            if (!w.sh)
                avformat_close_input(&w.fc);
        }
    }
    if (!w.sh) {
        w.d = open_demux(t, &w.sh);
        duration = w.d ? w.d->duration : 0;
        if (!w.d) {
            MP_WARN(t, "Could not open %s\n", t->url);
            goto out;
        }
    }
    if (t->auto_workers)
        t->workers = auto_workers(t->url, t->nsegs);
    t->sh = w.sh;
    setup(t, duration, w.sh);
    if (t->direct) {
        t->kfidx = talloc_array(t, int, t->count);
        for (int i = 0; i < t->count; i++)
            t->kfidx[i] = direct_entry(w.fc->streams[t->vindex], i * t->step);
    }
    if (!worker_open(t, &w)) {
        MP_WARN(t, "Could not open decoder for %s\n", w.sh->codec->codec);
        worker_close(&w);
        goto out;
    }
    if (t->state[0] != SLOT_EMPTY)
        talloc_free(grab(t, w.d, w.fc, w.sh, w.avctx, w.sws, w.pkt, w.frame, 0));

    double start = mp_time_sec();
    mp_mutex_lock(&t->lock);
    t->ready = true;
    t->changed = true;
    MP_VERBOSE(t, "%dx%d, %d thumbnails every %.1fs, %d workers\n",
               t->w, t->h, t->count, t->step, t->workers);
    t->wakeup(t->wakeup_ctx);
    mp_mutex_unlock(&t->lock);

    for (int n = 1; n < t->workers; n++) {
        if (mp_thread_create(&t->threads[t->nthreads], worker_thread, t) == 0)
            t->nthreads++;
    }
    work(t, &w);
    for (int n = 0; n < t->nthreads; n++)
        mp_thread_join(t->threads[n]);
    MP_VERBOSE(t, "Done in %.2fs\n", mp_time_sec() - start);
    worker_close(&w);

out:
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
    bool external = opts->thumbnail_url && opts->thumbnail_url[0];
    if (!opts->thumbnails)
        return;
    if (!external && (!mpctx->stream_open_filename || !mpctx->vo_chain ||
                      mpctx->vo_chain->is_coverart || !mpctx->demuxer ||
                      !mpctx->demuxer->seekable))
        return;

    struct thumbnailer *t = talloc_zero(NULL, struct thumbnailer);
    t->global = mpctx->global;
    t->log = mp_log_new(t, mpctx->log, "thumbnail");
    t->url = talloc_strdup(t, external ? opts->thumbnail_url : mpctx->stream_open_filename);
    t->stream_flags = mpctx->playing ? mpctx->playing->stream_flags : 0;
    t->rebase = opts->rebase_start_time;
    t->width = opts->thumbnail_width;
    t->interval = opts->thumbnail_interval;
    t->max_count = opts->thumbnail_max;
    t->memory = (int64_t)opts->thumbnail_memory * 1024 * 1024;
    t->sweep = opts->thumbnail_sweep;
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
    t->auto_workers = opts->thumbnail_workers <= 0;
    t->workers = opts->thumbnail_workers;
    t->threads = talloc_zero_array(t, mp_thread, 16);
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

void mp_thumbnails_file_loaded(struct MPContext *mpctx)
{
    struct thumbnailer *t = mpctx->thumbnailer;
    const char *url = mpctx->opts->thumbnail_url;
    if (t && url && url[0] && !strcmp(t->url, url))
        return;
    mp_thumbnails_start(mpctx);
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
        mp_mutex_destroy(&t->lock);
        mp_cond_destroy(&t->cond);
        av_dict_free(&t->net);
        av_free(t->init);
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
    mp_cond_broadcast(&t->cond);
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
        mp_cond_broadcast(&t->cond);
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
        node_map_add_int64(res, "count", t->cap);
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
        mp_cond_broadcast(&t->cond);
    }
    if (t->state[want] == SLOT_EMPTY && t->want != want) {
        t->want = want;
        mp_cond_broadcast(&t->cond);
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
