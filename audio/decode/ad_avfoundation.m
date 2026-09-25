/*
 * Apple AudioConverter based audio decoder.
 *
 * AudioConverter is the low-level system decoder. It is intentionally used
 * before AVSampleBufferAudioRenderer: the renderer is an output object and
 * cannot expose decoded samples to mpv's audio filter graph. This decoder
 * therefore keeps mpv's normal PCM/filter/AO contract while moving the codec
 * decode itself to the Apple media stack.
 */

#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <AudioToolbox/AudioToolbox.h>

#include "audio/aframe.h"
#include "audio/chmap.h"
#include "audio/format.h"
#include "audio/out/ao_coreaudio_chmap.h"
#include "common/av_common.h"
#include "common/codecs.h"
#include "common/msg.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "filters/f_decoder_wrapper.h"
#include "filters/filter_internal.h"

#define MAX_SAMPLES 8192
#define MAX_CONSECUTIVE_ERRORS 16

static const OSStatus input_drained = 'mpdr';

struct input_state {
    const uint8_t *data;
    size_t size;
    int channels;
    AudioStreamPacketDescription packet_description;
};

struct priv {
    AudioConverterRef converter;
    struct mp_chmap channels;
    int samplerate;
    int errors;

    struct mp_decoder public;
};

static bool codec_format(enum AVCodecID id, AudioFormatID *format)
{
    switch (id) {
    case AV_CODEC_ID_AAC:
        *format = kAudioFormatMPEG4AAC;
        return true;
    case AV_CODEC_ID_AC3:
        *format = kAudioFormatAC3;
        return true;
    case AV_CODEC_ID_EAC3:
        *format = kAudioFormatEnhancedAC3;
        return true;
    case AV_CODEC_ID_MP3:
        *format = kAudioFormatMPEGLayer3;
        return true;
    default:
        return false;
    }
}

static int codec_frames_per_packet(struct mp_codec_params *codec)
{
    switch (mp_codec_to_av_codec_id(codec->codec)) {
    case AV_CODEC_ID_AAC:  return 1024;
    case AV_CODEC_ID_MP3:  return codec->samplerate < 32000 ? 576 : 1152;
    case AV_CODEC_ID_AC3:  return 1536;
    case AV_CODEC_ID_EAC3: return 1536;
    default:               return 0;
    }
}

static uint8_t *put_descriptor(uint8_t *p, int tag, int size)
{
    *p++ = tag;
    for (int i = 3; i > 0; i--)
        *p++ = (size >> (7 * i)) | 0x80;
    *p++ = size & 0x7f;
    return p;
}

static uint8_t *aac_esds_cookie(const uint8_t *asc, int asc_size, int *out_size)
{
    int size = 5 + 3 + 5 + 13 + 5 + asc_size;
    uint8_t *cookie = talloc_zero_size(NULL, size);
    uint8_t *p = put_descriptor(cookie, 0x03, 3 + 5 + 13 + 5 + asc_size);
    p += 3;
    p = put_descriptor(p, 0x04, 13 + 5 + asc_size);
    *p++ = 0x40;
    *p++ = 0x15;
    p += 11;
    p = put_descriptor(p, 0x05, asc_size);
    memcpy(p, asc, asc_size);
    *out_size = size;
    return cookie;
}

static OSStatus input_callback(AudioConverterRef converter,
                               UInt32 *io_number_data_packets,
                               AudioBufferList *io_data,
                               AudioStreamPacketDescription **out_packet_desc,
                               void *user_data)
{
    struct input_state *input = user_data;
    if (!input->data || !input->size) {
        *io_number_data_packets = 0;
        return input_drained;
    }

    io_data->mNumberBuffers = 1;
    io_data->mBuffers[0].mNumberChannels = input->channels;
    io_data->mBuffers[0].mDataByteSize = input->size;
    io_data->mBuffers[0].mData = (void *)input->data;
    *io_number_data_packets = 1;
    input->packet_description = (AudioStreamPacketDescription){
        .mDataByteSize = input->size,
    };
    if (out_packet_desc)
        *out_packet_desc = &input->packet_description;
    input->data = NULL;
    input->size = 0;
    return noErr;
}

static void read_channel_layout(struct mp_filter *da)
{
    struct priv *p = da->priv;
    UInt32 size = 0;
    if (AudioConverterGetPropertyInfo(p->converter, kAudioConverterOutputChannelLayout,
                                      &size, NULL) != noErr || !size)
        return;

    AudioChannelLayout *layout = talloc_size(NULL, size);
    if (AudioConverterGetProperty(p->converter, kAudioConverterOutputChannelLayout,
                                  &size, layout) != noErr)
        goto done;

    if (layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap) {
        struct mp_chmap map = {0};
        mp_chmap_from_waveext(&map, layout->mChannelBitmap);
        if (map.num == p->channels.num)
            p->channels = map;
        goto done;
    }

    if (layout->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions) {
        AudioChannelLayoutTag tag = layout->mChannelLayoutTag;
        talloc_free(layout);
        layout = NULL;
        if (AudioFormatGetPropertyInfo(kAudioFormatProperty_ChannelLayoutForTag,
                                       sizeof(tag), &tag, &size) != noErr)
            goto done;
        layout = talloc_size(NULL, size);
        if (AudioFormatGetProperty(kAudioFormatProperty_ChannelLayoutForTag,
                                   sizeof(tag), &tag, &size, layout) != noErr)
            goto done;
    }

    if (layout->mNumberChannelDescriptions != p->channels.num)
        goto done;
    struct mp_chmap map = {.num = p->channels.num};
    for (int n = 0; n < map.num; n++) {
        int speaker = ca_label_to_mp_speaker_id(
            layout->mChannelDescriptions[n].mChannelLabel);
        if (speaker < 0)
            goto done;
        map.speaker[n] = speaker;
    }
    if (mp_chmap_is_valid(&map))
        p->channels = map;

done:
    talloc_free(layout);
    MP_VERBOSE(da, "AudioConverter output layout: %s\n",
               mp_chmap_to_str(&p->channels));
}

static void destroy(struct mp_filter *da)
{
    struct priv *p = da->priv;
    if (p->converter)
        AudioConverterDispose(p->converter);
}

static void reset(struct mp_filter *da)
{
    struct priv *p = da->priv;
    if (p->converter)
        AudioConverterReset(p->converter);
    p->errors = 0;
}

static bool decode_packet(struct mp_filter *da, struct demux_packet *packet,
                          struct mp_aframe **out)
{
    struct priv *p = da->priv;
    size_t bytes = (size_t)MAX_SAMPLES * p->channels.num * sizeof(float);
    struct mp_aframe *frame = mp_aframe_create();
    mp_aframe_set_format(frame, AF_FORMAT_FLOAT);
    mp_aframe_set_chmap(frame, &p->channels);
    mp_aframe_set_rate(frame, p->samplerate);
    if (!mp_aframe_alloc_data(frame, MAX_SAMPLES)) {
        talloc_free(frame);
        return false;
    }

    AudioBufferList output = {
        .mNumberBuffers = 1,
        .mBuffers = {{
            .mNumberChannels = p->channels.num,
            .mDataByteSize = (UInt32)bytes,
            .mData = mp_aframe_get_data_rw(frame)[0],
        }},
    };
    UInt32 samples = MAX_SAMPLES;
    struct input_state input = {
        .data = packet->buffer,
        .size = packet->len,
        .channels = p->channels.num,
    };

    OSStatus err = AudioConverterFillComplexBuffer(
        p->converter, input_callback, &input, &samples, &output, NULL);
    if (err != noErr && err != input_drained) {
        talloc_free(frame);
        return false;
    }
    if (!samples) {
        talloc_free(frame);
        *out = NULL;
        return true;
    }

    mp_aframe_set_size(frame, samples);
    double pts = packet->pts != MP_NOPTS_VALUE ? packet->pts : packet->dts;
    mp_aframe_set_pts(frame, pts);
    *out = frame;
    return true;
}

static void process(struct mp_filter *da)
{
    struct priv *p = da->priv;
    if (!mp_pin_can_transfer_data(da->ppins[1], da->ppins[0]))
        return;

    struct mp_frame in = mp_pin_out_read(da->ppins[0]);
    if (in.type == MP_FRAME_EOF) {
        mp_pin_in_write(da->ppins[1], in);
        return;
    }
    if (in.type != MP_FRAME_PACKET) {
        if (in.type) {
            MP_ERR(da, "Apple native decoder received an invalid frame\n");
            mp_frame_unref(&in);
            mp_filter_internal_mark_failed(da);
        }
        return;
    }

    struct demux_packet *packet = in.data;
    struct mp_aframe *out = NULL;
    bool ok = decode_packet(da, packet, &out);
    talloc_free(packet);

    if (!ok) {
        MP_WARN(da, "Apple AudioConverter could not decode a packet\n");
        if (++p->errors >= MAX_CONSECUTIVE_ERRORS) {
            mp_filter_internal_mark_failed(da);
            return;
        }
    } else {
        p->errors = 0;
    }

    if (out)
        mp_pin_in_write(da->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, out));
    mp_filter_internal_mark_progress(da);
}

static bool init(struct mp_filter *da, struct mp_codec_params *codec)
{
    struct priv *p = da->priv;
    AudioFormatID input_id;
    enum AVCodecID id = mp_codec_to_av_codec_id(codec->codec);
    if (!codec_format(id, &input_id))
        return false;

    const uint8_t *extra = codec->extradata;
    int extra_size = extra ? codec->extradata_size : 0;
    if (!extra_size && codec->lav_codecpar) {
        extra = codec->lav_codecpar->extradata;
        extra_size = extra ? codec->lav_codecpar->extradata_size : 0;
    }

    bool aac = id == AV_CODEC_ID_AAC;
    if (aac && !extra_size) {
        MP_VERBOSE(da, "AAC without AudioSpecificConfig is left to FFmpeg\n");
        return false;
    }

    p->samplerate = codec->samplerate;
    p->channels = codec->channels;
    if (p->samplerate <= 0 || p->channels.num <= 0 ||
        p->channels.num > MP_NUM_CHANNELS)
        return false;

    AudioStreamBasicDescription input = {
        .mSampleRate = p->samplerate,
        .mFormatID = input_id,
        .mFramesPerPacket = codec_frames_per_packet(codec),
        .mChannelsPerFrame = p->channels.num,
    };
    AudioStreamBasicDescription output = {
        .mSampleRate = p->samplerate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagsNativeFloatPacked,
        .mBytesPerPacket = p->channels.num * sizeof(float),
        .mFramesPerPacket = 1,
        .mBytesPerFrame = p->channels.num * sizeof(float),
        .mChannelsPerFrame = p->channels.num,
        .mBitsPerChannel = 32,
    };

    if (AudioConverterNew(&input, &output, &p->converter) != noErr)
        return false;

    const void *cookie = extra;
    int cookie_size = extra_size;
    uint8_t *esds = NULL;
    if (aac) {
        esds = aac_esds_cookie(extra, extra_size, &cookie_size);
        cookie = esds;
    }
    if (cookie && cookie_size > 0 &&
        AudioConverterSetProperty(p->converter,
            kAudioConverterDecompressionMagicCookie, cookie_size, cookie) != noErr)
    {
        MP_WARN(da, "Apple decoder rejected codec extradata\n");
        talloc_free(esds);
        return false;
    }
    talloc_free(esds);

    read_channel_layout(da);
    return true;
}

static const struct mp_filter_info ad_avfoundation_filter = {
    .name = "ad_avfoundation",
    .priv_size = sizeof(struct priv),
    .process = process,
    .reset = reset,
    .destroy = destroy,
};

static struct mp_decoder *create(struct mp_filter *parent,
                                 struct mp_codec_params *codec,
                                 const char *decoder)
{
    struct mp_filter *da = mp_filter_create(parent, &ad_avfoundation_filter);
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
    codec->decoder_desc = "Apple AudioConverter";
    return &p->public;
}

static void add_decoders(struct mp_decoder_list *list)
{
    mp_add_decoder(list, "aac", "avfoundation_aac", "Apple AudioConverter AAC");
    mp_add_decoder(list, "ac3", "avfoundation_ac3", "Apple AudioConverter AC-3");
    mp_add_decoder(list, "eac3", "avfoundation_eac3", "Apple AudioConverter E-AC-3");
    mp_add_decoder(list, "mp3", "avfoundation_mp3", "Apple AudioConverter MP3");
}

const struct mp_decoder_fns ad_avfoundation = {
    .create = create,
    .add_decoders = add_decoders,
};
