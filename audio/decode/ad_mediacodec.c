/*
 * Android MediaCodec audio decoder.
 *
 * This is deliberately a decoder, not an AudioTrack wrapper. Compressed
 * packets are submitted to the device codec and the decoded PCM is returned
 * as an mp_aframe, preserving mpv's normal audio filter and output contracts.
 */

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <libavcodec/avcodec.h>

#include "audio/aframe.h"
#include "audio/chmap.h"
#include "audio/format.h"
#include "common/av_common.h"
#include "common/codecs.h"
#include "common/msg.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "filters/f_decoder_wrapper.h"
#include "filters/filter_internal.h"
#include "osdep/threads.h"

#define MAX_BUFFERS 64
#define PTS_OFFSET_US (INT64_C(1) << 40)

struct codec_api {
    void *handle;
    AMediaCodec *(*create_decoder_by_type)(const char *mime);
    media_status_t (*configure)(AMediaCodec *, const AMediaFormat *, ANativeWindow *,
                                AMediaCrypto *, uint32_t);
    media_status_t (*start)(AMediaCodec *);
    media_status_t (*stop)(AMediaCodec *);
    media_status_t (*delete_codec)(AMediaCodec *);
    media_status_t (*flush)(AMediaCodec *);
    uint8_t *(*get_input_buffer)(AMediaCodec *, size_t, size_t *);
    media_status_t (*queue_input_buffer)(AMediaCodec *, size_t, off_t, size_t,
                                         uint64_t, uint32_t);
    uint8_t *(*get_output_buffer)(AMediaCodec *, size_t, size_t *);
    media_status_t (*release_output_buffer)(AMediaCodec *, size_t, bool);
    AMediaFormat *(*get_output_format)(AMediaCodec *);
    media_status_t (*set_async_callback)(AMediaCodec *, AMediaCodecOnAsyncNotifyCallback,
                                         void *);
    AMediaFormat *(*format_new)(void);
    media_status_t (*format_delete)(AMediaFormat *);
    void (*format_set_string)(AMediaFormat *, const char *, const char *);
    void (*format_set_int32)(AMediaFormat *, const char *, int32_t);
    void (*format_set_buffer)(AMediaFormat *, const char *, const void *, size_t);
    bool (*format_get_int32)(AMediaFormat *, const char *, int32_t *);
};

struct output_buffer {
    int32_t index;
    AMediaCodecBufferInfo info;
};

struct priv {
    struct mp_filter *f;
    struct codec_api api;
    AMediaCodec *decoder;
    int samplerate;
    struct mp_chmap channels;
    int format;
    struct demux_packet *pending;
    bool pending_eof;
    bool sent_eof;
    bool eos_received;
    bool output_eof;

    mp_mutex lock;
    int32_t inputs[MAX_BUFFERS];
    int num_inputs;
    struct output_buffer outputs[MAX_BUFFERS];
    int num_outputs;
    bool format_changed;
    bool codec_error;

    struct mp_decoder public;
};

static bool load_api(struct mp_filter *da, struct codec_api *api)
{
    api->handle = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
    if (!api->handle)
        return false;

#define LOAD(name, symbol) do { \
    *(void **)(&api->name) = dlsym(api->handle, symbol); \
    if (!api->name) goto error; \
} while (0)
    LOAD(create_decoder_by_type, "AMediaCodec_createDecoderByType");
    LOAD(configure, "AMediaCodec_configure");
    LOAD(start, "AMediaCodec_start");
    LOAD(stop, "AMediaCodec_stop");
    LOAD(delete_codec, "AMediaCodec_delete");
    LOAD(flush, "AMediaCodec_flush");
    LOAD(get_input_buffer, "AMediaCodec_getInputBuffer");
    LOAD(queue_input_buffer, "AMediaCodec_queueInputBuffer");
    LOAD(get_output_buffer, "AMediaCodec_getOutputBuffer");
    LOAD(release_output_buffer, "AMediaCodec_releaseOutputBuffer");
    LOAD(get_output_format, "AMediaCodec_getOutputFormat");
    LOAD(set_async_callback, "AMediaCodec_setAsyncNotifyCallback");
    LOAD(format_new, "AMediaFormat_new");
    LOAD(format_delete, "AMediaFormat_delete");
    LOAD(format_set_string, "AMediaFormat_setString");
    LOAD(format_set_int32, "AMediaFormat_setInt32");
    LOAD(format_set_buffer, "AMediaFormat_setBuffer");
    LOAD(format_get_int32, "AMediaFormat_getInt32");
#undef LOAD
    return true;

error:
    dlclose(api->handle);
    *api = (struct codec_api){0};
    MP_VERBOSE(da, "Android MediaCodec audio API is unavailable\n");
    return false;
}

static const char *codec_mime(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_AAC:    return "audio/mp4a-latm";
    case AV_CODEC_ID_AC3:    return "audio/ac3";
    case AV_CODEC_ID_EAC3:   return "audio/eac3";
    case AV_CODEC_ID_MP3:    return "audio/mpeg";
    case AV_CODEC_ID_OPUS:   return "audio/opus";
    case AV_CODEC_ID_VORBIS: return "audio/vorbis";
    case AV_CODEC_ID_FLAC:   return "audio/flac";
    default:                 return NULL;
    }
}

static void on_input(AMediaCodec *codec, void *userdata, int32_t index)
{
    struct priv *p = userdata;
    mp_mutex_lock(&p->lock);
    if (p->num_inputs < MAX_BUFFERS)
        p->inputs[p->num_inputs++] = index;
    mp_mutex_unlock(&p->lock);
    mp_filter_wakeup(p->f);
}

static void on_output(AMediaCodec *codec, void *userdata, int32_t index,
                      AMediaCodecBufferInfo *info)
{
    struct priv *p = userdata;
    mp_mutex_lock(&p->lock);
    if (p->num_outputs < MAX_BUFFERS) {
        p->outputs[p->num_outputs++] = (struct output_buffer){index, *info};
    } else {
        p->api.release_output_buffer(codec, index, false);
    }
    mp_mutex_unlock(&p->lock);
    mp_filter_wakeup(p->f);
}

static void on_format(AMediaCodec *codec, void *userdata, AMediaFormat *format)
{
    struct priv *p = userdata;
    mp_mutex_lock(&p->lock);
    p->format_changed = true;
    mp_mutex_unlock(&p->lock);
    mp_filter_wakeup(p->f);
}

static void on_error(AMediaCodec *codec, void *userdata, media_status_t error,
                     int32_t action_code, const char *detail)
{
    struct priv *p = userdata;
    mp_mutex_lock(&p->lock);
    p->codec_error = true;
    mp_mutex_unlock(&p->lock);
    mp_filter_wakeup(p->f);
}

static void destroy(struct mp_filter *da)
{
    struct priv *p = da->priv;
    if (p->decoder) {
        p->api.stop(p->decoder);
        p->api.delete_codec(p->decoder);
    }
    if (p->api.handle)
        dlclose(p->api.handle);
    talloc_free(p->pending);
    mp_mutex_destroy(&p->lock);
}

static void reset(struct mp_filter *da)
{
    struct priv *p = da->priv;
    TA_FREEP(&p->pending);
    p->pending_eof = false;
    p->sent_eof = false;
    p->eos_received = false;
    p->output_eof = false;

    p->api.flush(p->decoder);
    mp_mutex_lock(&p->lock);
    p->num_inputs = 0;
    p->num_outputs = 0;
    mp_mutex_unlock(&p->lock);
    if (p->api.start(p->decoder) != AMEDIA_OK) {
        MP_ERR(da, "MediaCodec failed to restart after flush\n");
        mp_mutex_lock(&p->lock);
        p->codec_error = true;
        mp_mutex_unlock(&p->lock);
    }
}

static void update_output_format(struct priv *p)
{
    AMediaFormat *format = p->api.get_output_format(p->decoder);
    if (!format)
        return;

    int32_t rate = 0, channels = 0, mask = 0, encoding = 0;
    if (p->api.format_get_int32(format, "sample-rate", &rate) && rate > 0)
        p->samplerate = rate;
    if (p->api.format_get_int32(format, "channel-count", &channels) &&
        channels > 0 && channels <= MP_NUM_CHANNELS)
    {
        mp_chmap_from_channels(&p->channels, channels);
        struct mp_chmap masked = {0};
        if (channels > 2 &&
            p->api.format_get_int32(format, "channel-mask", &mask) && mask > 0)
        {
            mp_chmap_from_waveext(&masked, (uint32_t)mask >> 2);
            if (masked.num == channels)
                p->channels = masked;
        }
    }
    p->format = AF_FORMAT_S16;
    if (p->api.format_get_int32(format, "pcm-encoding", &encoding)) {
        if (encoding == 4)
            p->format = AF_FORMAT_FLOAT;
        else if (encoding == 3)
            p->format = AF_FORMAT_U8;
    }
    p->api.format_delete(format);

    MP_VERBOSE(p->f, "MediaCodec output: %d Hz, %s, %s\n", p->samplerate,
               mp_chmap_to_str(&p->channels), af_fmt_to_str(p->format));
}

static struct mp_aframe *receive_output(struct priv *p, struct output_buffer *buf)
{
    AMediaCodecBufferInfo *info = &buf->info;
    size_t capacity = 0;
    uint8_t *data = p->api.get_output_buffer(p->decoder, buf->index, &capacity);
    if (!data || info->offset < 0 || (size_t)info->offset > capacity ||
        (size_t)info->size > capacity - info->offset)
        return NULL;

    int bytes = af_fmt_to_bytes(p->format);
    int channels = p->channels.num;
    if (!bytes || !channels || info->size < bytes * channels)
        return NULL;

    int samples = info->size / (bytes * channels);
    struct mp_aframe *out = mp_aframe_create();
    mp_aframe_set_format(out, p->format);
    mp_aframe_set_chmap(out, &p->channels);
    mp_aframe_set_rate(out, p->samplerate);
    if (!mp_aframe_alloc_data(out, samples)) {
        talloc_free(out);
        return NULL;
    }
    memcpy(mp_aframe_get_data_rw(out)[0], data + info->offset,
           (size_t)samples * bytes * channels);
    if (info->presentationTimeUs > 0)
        mp_aframe_set_pts(out, (info->presentationTimeUs - PTS_OFFSET_US) / 1e6);
    return out;
}

static int64_t packet_pts_us(struct demux_packet *packet)
{
    double pts = packet->pts != MP_NOPTS_VALUE ? packet->pts : packet->dts;
    if (pts == MP_NOPTS_VALUE)
        return 0;
    return llrint(pts * 1e6) + PTS_OFFSET_US;
}

static bool queue_input(struct mp_filter *da, int32_t index)
{
    struct priv *p = da->priv;

    if (p->pending_eof) {
        if (p->api.queue_input_buffer(p->decoder, index, 0, 0, 0,
                AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != AMEDIA_OK)
            return false;
        p->pending_eof = false;
        p->sent_eof = true;
        return true;
    }

    struct demux_packet *packet = p->pending;
    size_t capacity = 0;
    uint8_t *buffer = p->api.get_input_buffer(p->decoder, index, &capacity);
    if (!buffer || packet->len > capacity) {
        MP_ERR(da, "MediaCodec input buffer is too small for audio packet\n");
        return false;
    }
    memcpy(buffer, packet->buffer, packet->len);
    media_status_t status = p->api.queue_input_buffer(
        p->decoder, index, 0, packet->len, packet_pts_us(packet), 0);
    if (status != AMEDIA_OK) {
        MP_ERR(da, "MediaCodec failed to queue an audio packet: %d\n", status);
        return false;
    }
    TA_FREEP(&p->pending);
    return true;
}

static void process(struct mp_filter *da)
{
    struct priv *p = da->priv;

    mp_mutex_lock(&p->lock);
    bool error = p->codec_error;
    bool format_changed = p->format_changed;
    p->format_changed = false;
    mp_mutex_unlock(&p->lock);

    if (error) {
        MP_ERR(da, "MediaCodec audio decoder reported an error\n");
        mp_filter_internal_mark_failed(da);
        return;
    }
    if (format_changed)
        update_output_format(p);

    if (p->output_eof || !mp_pin_in_needs_data(da->ppins[1]))
        return;

    if (p->eos_received) {
        p->output_eof = true;
        mp_pin_in_write(da->ppins[1], MP_EOF_FRAME);
        return;
    }

    struct output_buffer out;
    mp_mutex_lock(&p->lock);
    bool have_output = p->num_outputs > 0;
    if (have_output) {
        out = p->outputs[0];
        MP_TARRAY_REMOVE_AT(p->outputs, p->num_outputs, 0);
    }
    mp_mutex_unlock(&p->lock);

    if (have_output) {
        struct mp_aframe *frame = NULL;
        if (!(out.info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) && out.info.size > 0)
            frame = receive_output(p, &out);
        p->api.release_output_buffer(p->decoder, out.index, false);
        bool eof = out.info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
        p->eos_received = eof;
        if (frame)
            mp_pin_in_write(da->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, frame));
        mp_filter_internal_mark_progress(da);
        return;
    }

    if (!p->pending && !p->pending_eof && !p->sent_eof) {
        struct mp_frame in = mp_pin_out_read(da->ppins[0]);
        if (in.type == MP_FRAME_EOF) {
            p->pending_eof = true;
        } else if (in.type == MP_FRAME_PACKET) {
            p->pending = in.data;
        } else if (in.type) {
            MP_ERR(da, "MediaCodec audio decoder received an invalid frame\n");
            mp_frame_unref(&in);
            mp_filter_internal_mark_failed(da);
            return;
        } else {
            return;
        }
    }

    if (!p->pending && !p->pending_eof)
        return;

    mp_mutex_lock(&p->lock);
    int32_t index = -1;
    if (p->num_inputs > 0) {
        index = p->inputs[0];
        MP_TARRAY_REMOVE_AT(p->inputs, p->num_inputs, 0);
    }
    mp_mutex_unlock(&p->lock);

    if (index < 0)
        return;

    if (!queue_input(da, index)) {
        mp_filter_internal_mark_failed(da);
        return;
    }
    mp_filter_internal_mark_progress(da);
}

static bool split_xiph_headers(const uint8_t *data, int size,
                               const uint8_t *out[3], int out_size[3])
{
    if (size >= 6 && data[0] == 0 && data[1] == 30) {
        for (int i = 0; i < 3; i++) {
            if (size < 2)
                return false;
            out_size[i] = (data[0] << 8) | data[1];
            data += 2;
            size -= 2;
            if (out_size[i] > size)
                return false;
            out[i] = data;
            data += out_size[i];
            size -= out_size[i];
        }
        return true;
    }

    if (size < 3 || data[0] != 2)
        return false;
    int pos = 1;
    for (int i = 0; i < 2; i++) {
        out_size[i] = 0;
        while (pos < size && data[pos] == 255)
            out_size[i] += data[pos++];
        if (pos >= size)
            return false;
        out_size[i] += data[pos++];
    }
    out_size[2] = size - pos - out_size[0] - out_size[1];
    if (out_size[2] < 0)
        return false;
    out[0] = data + pos;
    out[1] = out[0] + out_size[0];
    out[2] = out[1] + out_size[1];
    return true;
}

static bool set_codec_config(struct priv *p, AMediaFormat *format,
                             struct mp_codec_params *codec)
{
    const uint8_t *extra = codec->extradata;
    int extra_size = extra ? codec->extradata_size : 0;
    if (!extra_size && codec->lav_codecpar) {
        extra = codec->lav_codecpar->extradata;
        extra_size = extra ? codec->lav_codecpar->extradata_size : 0;
    }

    switch (mp_codec_to_av_codec_id(codec->codec)) {
    case AV_CODEC_ID_AAC:
        if (extra_size > 0) {
            p->api.format_set_buffer(format, "csd-0", extra, extra_size);
        } else {
            p->api.format_set_int32(format, "is-adts", 1);
        }
        return true;
    case AV_CODEC_ID_OPUS: {
        if (extra_size < 19 || memcmp(extra, "OpusHead", 8) != 0)
            return false;
        int pre_skip = extra[10] | (extra[11] << 8);
        int64_t codec_delay = (int64_t)pre_skip * 1000000000 / 48000;
        int64_t seek_preroll = 80000000;
        p->api.format_set_buffer(format, "csd-0", extra, extra_size);
        p->api.format_set_buffer(format, "csd-1", &codec_delay, sizeof(codec_delay));
        p->api.format_set_buffer(format, "csd-2", &seek_preroll, sizeof(seek_preroll));
        return true;
    }
    case AV_CODEC_ID_VORBIS: {
        const uint8_t *headers[3];
        int sizes[3];
        if (!split_xiph_headers(extra, extra_size, headers, sizes))
            return false;
        p->api.format_set_buffer(format, "csd-0", headers[0], sizes[0]);
        p->api.format_set_buffer(format, "csd-1", headers[2], sizes[2]);
        return true;
    }
    case AV_CODEC_ID_FLAC: {
        if (extra_size >= 4 && memcmp(extra, "fLaC", 4) == 0) {
            p->api.format_set_buffer(format, "csd-0", extra, extra_size);
            return true;
        }
        if (extra_size < 34)
            return false;
        uint8_t csd[42] = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
        memcpy(csd + 8, extra, 34);
        p->api.format_set_buffer(format, "csd-0", csd, sizeof(csd));
        return true;
    }
    default:
        return true;
    }
}

static bool init(struct mp_filter *da, struct mp_codec_params *codec)
{
    struct priv *p = da->priv;
    const char *mime = codec_mime(mp_codec_to_av_codec_id(codec->codec));
    if (!mime || codec->samplerate <= 0 || codec->channels.num <= 0)
        return false;
    if (!load_api(da, &p->api))
        return false;

    p->samplerate = codec->samplerate;
    p->channels = codec->channels;
    p->format = AF_FORMAT_S16;

    AMediaFormat *format = p->api.format_new();
    if (!format)
        return false;
    p->api.format_set_string(format, "mime", mime);
    p->api.format_set_int32(format, "sample-rate", p->samplerate);
    p->api.format_set_int32(format, "channel-count", p->channels.num);
    if (!set_codec_config(p, format, codec)) {
        MP_VERBOSE(da, "Unsupported codec configuration for MediaCodec\n");
        p->api.format_delete(format);
        return false;
    }

    p->decoder = p->api.create_decoder_by_type(mime);
    if (!p->decoder) {
        p->api.format_delete(format);
        return false;
    }

    AMediaCodecOnAsyncNotifyCallback callbacks = {
        .onAsyncInputAvailable = on_input,
        .onAsyncOutputAvailable = on_output,
        .onAsyncFormatChanged = on_format,
        .onAsyncError = on_error,
    };
    media_status_t status = p->api.set_async_callback(p->decoder, callbacks, p);
    if (status == AMEDIA_OK)
        status = p->api.configure(p->decoder, format, NULL, NULL, 0);
    p->api.format_delete(format);
    return status == AMEDIA_OK && p->api.start(p->decoder) == AMEDIA_OK;
}

static const struct mp_filter_info ad_mediacodec_filter = {
    .name = "ad_mediacodec",
    .priv_size = sizeof(struct priv),
    .process = process,
    .reset = reset,
    .destroy = destroy,
};

static struct mp_decoder *create(struct mp_filter *parent,
                                 struct mp_codec_params *codec,
                                 const char *decoder)
{
    struct mp_filter *da = mp_filter_create(parent, &ad_mediacodec_filter);
    if (!da)
        return NULL;
    mp_filter_add_pin(da, MP_PIN_IN, "in");
    mp_filter_add_pin(da, MP_PIN_OUT, "out");
    da->log = mp_log_new(da, parent->log, NULL);
    struct priv *p = da->priv;
    p->f = da;
    p->public.f = da;
    mp_mutex_init(&p->lock);
    if (!init(da, codec)) {
        talloc_free(da);
        return NULL;
    }
    codec->decoder_desc = "Android MediaCodec audio decoder";
    return &p->public;
}

static void add_decoders(struct mp_decoder_list *list)
{
    mp_add_decoder(list, "aac", "mediacodec_aac", "Android MediaCodec AAC");
    mp_add_decoder(list, "ac3", "mediacodec_ac3", "Android MediaCodec AC-3");
    mp_add_decoder(list, "eac3", "mediacodec_eac3", "Android MediaCodec E-AC-3");
    mp_add_decoder(list, "mp3", "mediacodec_mp3", "Android MediaCodec MP3");
    mp_add_decoder(list, "opus", "mediacodec_opus", "Android MediaCodec Opus");
    mp_add_decoder(list, "vorbis", "mediacodec_vorbis", "Android MediaCodec Vorbis");
    mp_add_decoder(list, "flac", "mediacodec_flac", "Android MediaCodec FLAC");
}

const struct mp_decoder_fns ad_mediacodec = {
    .create = create,
    .add_decoders = add_decoders,
};
