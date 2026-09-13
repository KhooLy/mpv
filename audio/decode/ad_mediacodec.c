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
#include "common/codecs.h"
#include "common/msg.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "filters/f_decoder_wrapper.h"
#include "filters/filter_internal.h"

struct codec_api {
    void *handle;
    AMediaCodec *(*create_decoder_by_type)(const char *mime);
    media_status_t (*configure)(AMediaCodec *, const AMediaFormat *, ANativeWindow *,
                                AMediaCrypto *, uint32_t);
    media_status_t (*start)(AMediaCodec *);
    media_status_t (*stop)(AMediaCodec *);
    void (*delete_codec)(AMediaCodec *);
    media_status_t (*flush)(AMediaCodec *);
    ssize_t (*dequeue_input_buffer)(AMediaCodec *, int64_t);
    uint8_t *(*get_input_buffer)(AMediaCodec *, size_t, size_t *);
    media_status_t (*queue_input_buffer)(AMediaCodec *, size_t, off_t, size_t,
                                         uint64_t, uint32_t);
    ssize_t (*dequeue_output_buffer)(AMediaCodec *, AMediaCodecBufferInfo *, int64_t);
    uint8_t *(*get_output_buffer)(AMediaCodec *, size_t, size_t *);
    media_status_t (*release_output_buffer)(AMediaCodec *, size_t, bool);
    AMediaFormat *(*get_output_format)(AMediaCodec *);
    AMediaFormat *(*format_new)(void);
    media_status_t (*format_delete)(AMediaFormat *);
    void (*format_set_string)(AMediaFormat *, const char *, const char *);
    void (*format_set_int32)(AMediaFormat *, const char *, int32_t);
    void (*format_set_buffer)(AMediaFormat *, const char *, const void *, size_t);
    bool (*format_get_int32)(AMediaFormat *, const char *, int32_t *);
};

struct priv {
    struct mp_codec_params *codec;
    struct codec_api api;
    AMediaCodec *decoder;
    int samplerate;
    struct mp_chmap channels;
    int format;
    struct demux_packet *pending;
    bool pending_eof;
    bool sent_eof;
    bool output_eof;
    bool input_error;

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
    LOAD(dequeue_input_buffer, "AMediaCodec_dequeueInputBuffer");
    LOAD(get_input_buffer, "AMediaCodec_getInputBuffer");
    LOAD(queue_input_buffer, "AMediaCodec_queueInputBuffer");
    LOAD(dequeue_output_buffer, "AMediaCodec_dequeueOutputBuffer");
    LOAD(get_output_buffer, "AMediaCodec_getOutputBuffer");
    LOAD(release_output_buffer, "AMediaCodec_releaseOutputBuffer");
    LOAD(get_output_format, "AMediaCodec_getOutputFormat");
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
    case AV_CODEC_ID_AAC:   return "audio/mp4a-latm";
    case AV_CODEC_ID_AC3:   return "audio/ac3";
    case AV_CODEC_ID_EAC3:  return "audio/eac3";
    case AV_CODEC_ID_MP3:   return "audio/mpeg";
    case AV_CODEC_ID_OPUS:  return "audio/opus";
    case AV_CODEC_ID_VORBIS:return "audio/vorbis";
    case AV_CODEC_ID_FLAC:  return "audio/flac";
    default:                return NULL;
    }
}

static void destroy(struct mp_filter *da)
{
    struct priv *p = da->priv;
    if (p->pending)
        talloc_free(p->pending);
    if (p->decoder) {
        p->api.stop(p->decoder);
        p->api.delete_codec(p->decoder);
    }
    if (p->api.handle)
        dlclose(p->api.handle);
}

static void reset(struct mp_filter *da)
{
    struct priv *p = da->priv;
    if (p->pending) {
        talloc_free(p->pending);
        p->pending = NULL;
    }
    p->pending_eof = false;
    p->sent_eof = false;
    p->output_eof = false;
    p->input_error = false;
    p->api.flush(p->decoder);
}

static void update_output_format(struct priv *p, AMediaFormat *format)
{
    int32_t rate = 0, channels = 0, encoding = 0;
    if (p->api.format_get_int32(format, "sample-rate", &rate) && rate > 0)
        p->samplerate = rate;
    if (p->api.format_get_int32(format, "channel-count", &channels) &&
        channels > 0 && channels <= 32)
        mp_chmap_from_channels(&p->channels, channels);
    if (p->api.format_get_int32(format, "pcm-encoding", &encoding)) {
        if (encoding == 4)
            p->format = AF_FORMAT_FLOAT;
        else if (encoding == 2)
            p->format = AF_FORMAT_S16;
        else if (encoding == 3)
            p->format = AF_FORMAT_U8;
    }
}

static struct mp_aframe *receive_output(struct mp_filter *da, size_t index,
                                        AMediaCodecBufferInfo *info)
{
    struct priv *p = da->priv;
    size_t capacity = 0;
    uint8_t *data = p->api.get_output_buffer(p->decoder, index, &capacity);
    if (!data || info->offset < 0 || (size_t)info->offset > capacity ||
        (size_t)info->size > capacity - info->offset)
        return NULL;

    data += info->offset;
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
    memcpy(mp_aframe_get_data_rw(out)[0], data,
           (size_t)samples * bytes * channels);
    mp_aframe_set_pts(out, info->presentationTimeUs / 1000000.0);
    return out;
}

static bool queue_pending(struct mp_filter *da)
{
    struct priv *p = da->priv;
    ssize_t index = p->api.dequeue_input_buffer(p->decoder, 10000);
    if (index < 0)
        return false;

    if (p->pending_eof) {
        media_status_t status = p->api.queue_input_buffer(
            p->decoder, index, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        if (status != AMEDIA_OK) {
            p->input_error = true;
            return false;
        }
        p->pending_eof = false;
        p->sent_eof = true;
        return true;
    }

    struct demux_packet *packet = p->pending;
    size_t capacity = 0;
    uint8_t *buffer = p->api.get_input_buffer(p->decoder, index, &capacity);
    if (!buffer || packet->len > capacity) {
        MP_ERR(da, "MediaCodec input buffer is too small for audio packet\n");
        p->input_error = true;
        return false;
    }
    memcpy(buffer, packet->buffer, packet->len);
    uint64_t pts = packet->pts == MP_NOPTS_VALUE ? 0 :
                   (uint64_t)llrint(packet->pts * 1000000.0);
    media_status_t status = p->api.queue_input_buffer(
        p->decoder, index, 0, packet->len, pts, 0);
    if (status != AMEDIA_OK) {
        MP_ERR(da, "MediaCodec failed to queue an audio packet: %d\n", status);
        p->input_error = true;
        return false;
    }
    talloc_free(p->pending);
    p->pending = NULL;
    return true;
}

static void process(struct mp_filter *da)
{
    struct priv *p = da->priv;
    if (!mp_pin_can_transfer_data(da->ppins[1], da->ppins[0]))
        return;

    ssize_t index;
    AMediaCodecBufferInfo info;
    index = p->api.dequeue_output_buffer(p->decoder, &info, 0);
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat *format = p->api.get_output_format(p->decoder);
        if (format) {
            update_output_format(p, format);
            p->api.format_delete(format);
        }
        mp_filter_internal_mark_progress(da);
        return;
    }
    if (index >= 0) {
        struct mp_aframe *out = NULL;
        if (!(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) && info.size > 0)
            out = receive_output(da, index, &info);
        p->api.release_output_buffer(p->decoder, index, false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
            p->output_eof = true;
        if (out) {
            mp_pin_in_write(da->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, out));
            return;
        }
        if (p->output_eof) {
            mp_pin_in_write(da->ppins[1], MP_EOF_FRAME);
            return;
        }
        mp_filter_internal_mark_progress(da);
        return;
    }

    if (p->output_eof) {
        mp_pin_in_write(da->ppins[1], MP_EOF_FRAME);
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
            mp_filter_internal_mark_failed(da);
            return;
        } else {
            return;
        }
    }

    if (!p->pending && !p->pending_eof)
        return;

    if (!queue_pending(da)) {
        if (p->input_error) {
            mp_filter_internal_mark_failed(da);
            return;
        }
        mp_filter_internal_mark_progress(da);
        return;
    }
    mp_filter_internal_mark_progress(da);
}

static bool init(struct mp_filter *da, struct mp_codec_params *codec)
{
    struct priv *p = da->priv;
    const char *mime = codec->lav_codecpar ?
                       codec_mime(codec->lav_codecpar->codec_id) : NULL;
    if (!mime || codec->samplerate <= 0 || codec->channels.num <= 0)
        return false;
    if (!load_api(da, &p->api))
        return false;

    p->codec = codec;
    p->samplerate = codec->samplerate;
    p->channels = codec->channels;
    p->format = AF_FORMAT_S16;
    p->decoder = p->api.create_decoder_by_type(mime);
    AMediaFormat *format = p->api.format_new();
    if (!p->decoder || !format) {
        if (format)
            p->api.format_delete(format);
        return false;
    }
    p->api.format_set_string(format, "mime", mime);
    p->api.format_set_int32(format, "sample-rate", p->samplerate);
    p->api.format_set_int32(format, "channel-count", p->channels.num);
    if (codec->extradata && codec->extradata_size > 0)
        p->api.format_set_buffer(format, "csd-0", codec->extradata,
                                 codec->extradata_size);

    media_status_t status = p->api.configure(p->decoder, format, NULL, NULL, 0);
    p->api.format_delete(format);
    if (status != AMEDIA_OK || p->api.start(p->decoder) != AMEDIA_OK)
        return false;
    return true;
}

static struct mp_decoder *create(struct mp_filter *parent,
                                 struct mp_codec_params *codec,
                                 const char *decoder)
{
    struct mp_filter *da = mp_filter_create(parent, &(const struct mp_filter_info){
        .name = "ad_mediacodec",
        .priv_size = sizeof(struct priv),
        .process = process,
        .reset = reset,
        .destroy = destroy,
    });
    if (!da)
        return NULL;
    mp_filter_add_pin(da, MP_PIN_IN, "in");
    mp_filter_add_pin(da, MP_PIN_OUT, "out");
    da->log = mp_log_new(da, parent->log, NULL);
    struct priv *p = da->priv;
    p->public.f = da;
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
