/*
 * Apple AudioConverter based audio decoder.
 *
 * AudioConverter is the low-level system decoder. It is intentionally used
 * before AVSampleBufferAudioRenderer: the renderer is an output object and
 * cannot expose decoded samples to mpv's audio filter graph. This decoder
 * therefore keeps mpv's normal PCM/filter/AO contract while moving the codec
 * decode itself to the Apple media stack.
 */

#include <string.h>

#include <libavcodec/avcodec.h>
#include <AudioToolbox/AudioToolbox.h>

#include "audio/aframe.h"
#include "audio/chmap.h"
#include "audio/format.h"
#include "common/codecs.h"
#include "common/msg.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "filters/f_decoder_wrapper.h"
#include "filters/filter_internal.h"

struct input_state {
    const uint8_t *data;
    size_t size;
    int channels;
    AudioStreamPacketDescription packet_description;
};

struct priv {
    struct mp_codec_params *codec;
    AudioConverterRef converter;
    AudioStreamBasicDescription output_format;
    struct mp_chmap channels;
    int samplerate;
    int max_samples;

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

static int codec_frames_per_packet(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_AAC:  return 1024;
    case AV_CODEC_ID_MP3:  return 1152;
    case AV_CODEC_ID_AC3:  return 1536;
    case AV_CODEC_ID_EAC3: return 1536;
    default:               return 1;
    }
}

static OSStatus input_callback(AudioConverterRef converter,
                               UInt32 *io_number_data_packets,
                               AudioBufferList *io_data,
                               AudioStreamPacketDescription **out_packet_desc,
                               void *user_data)
{
    struct input_state *input = user_data;
    if (!input->data || !input->size || !*io_number_data_packets)
        return kAudioConverterErr_EndOfData;

    io_data->mNumberBuffers = 1;
    io_data->mBuffers[0].mNumberChannels = input->channels;
    io_data->mBuffers[0].mDataByteSize = input->size;
    io_data->mBuffers[0].mData = (void *)input->data;
    *io_number_data_packets = 1;
    input->packet_description = (AudioStreamPacketDescription){
        .mStartOffset = 0,
        .mVariableFramesInPacket = 0,
        .mDataByteSize = input->size,
    };
    if (out_packet_desc)
        *out_packet_desc = &input->packet_description;
    input->data = NULL;
    input->size = 0;
    return noErr;
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
}

static struct mp_aframe *decode_packet(struct mp_filter *da,
                                        struct demux_packet *packet)
{
    struct priv *p = da->priv;
    uint8_t *buffer = talloc_size(NULL,
                                  (size_t)p->max_samples *
                                  p->channels.num * sizeof(float));
    MP_HANDLE_OOM(buffer);

    AudioBufferList output = {
        .mNumberBuffers = 1,
        .mBuffers = {{
            .mNumberChannels = p->channels.num,
            .mDataByteSize = (UInt32)((size_t)p->max_samples *
                                       p->channels.num * sizeof(float)),
            .mData = buffer,
        }},
    };
    UInt32 samples = p->max_samples;
    struct input_state input = {
        .data = packet->buffer,
        .size = packet->len,
        .channels = p->channels.num,
    };

    OSStatus err = AudioConverterFillComplexBuffer(
        p->converter, input_callback, &input, &samples, &output, NULL);
    if (err != noErr || samples == 0) {
        MP_ERR(da, "Apple AudioConverter failed: %d\n", (int)err);
        talloc_free(buffer);
        return NULL;
    }

    struct mp_aframe *out = mp_aframe_create();
    mp_aframe_set_format(out, AF_FORMAT_FLOAT);
    mp_aframe_set_chmap(out, &p->channels);
    mp_aframe_set_rate(out, p->samplerate);
    if (!mp_aframe_alloc_data(out, samples)) {
        talloc_free(buffer);
        talloc_free(out);
        return NULL;
    }
    memcpy(mp_aframe_get_data_rw(out)[0], buffer,
           (size_t)samples * p->channels.num * sizeof(float));
    mp_aframe_set_pts(out, packet->pts);
    talloc_free(buffer);
    return out;
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
            mp_filter_internal_mark_failed(da);
        }
        return;
    }

    struct demux_packet *packet = in.data;
    struct mp_aframe *out = decode_packet(da, packet);
    talloc_free(packet);
    if (!out) {
        mp_filter_internal_mark_failed(da);
        return;
    }
    mp_pin_in_write(da->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, out));
}

static bool init(struct mp_filter *da, struct mp_codec_params *codec)
{
    struct priv *p = da->priv;
    AudioFormatID input_id;
    if (!codec->lav_codecpar ||
        !codec_format(codec->lav_codecpar->codec_id, &input_id))
        return false;

    p->codec = codec;
    p->samplerate = codec->samplerate;
    p->channels = codec->channels;
    if (p->samplerate <= 0 || p->channels.num <= 0 || p->channels.num > 32)
        return false;

    AudioStreamBasicDescription input = {
        .mSampleRate = p->samplerate,
        .mFormatID = input_id,
        .mFramesPerPacket = codec_frames_per_packet(codec->lav_codecpar->codec_id),
        .mChannelsPerFrame = p->channels.num,
    };
    p->output_format = (AudioStreamBasicDescription){
        .mSampleRate = p->samplerate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagsNativeFloatPacked,
        .mBytesPerPacket = p->channels.num * sizeof(float),
        .mFramesPerPacket = 1,
        .mBytesPerFrame = p->channels.num * sizeof(float),
        .mChannelsPerFrame = p->channels.num,
        .mBitsPerChannel = 32,
    };

    if (AudioConverterNew(&input, &p->output_format, &p->converter) != noErr)
        return false;

    if (codec->extradata && codec->extradata_size > 0 &&
        AudioConverterSetProperty(p->converter,
            kAudioConverterDecompressionMagicCookie,
            codec->extradata_size, codec->extradata) != noErr) {
        MP_WARN(da, "Apple decoder rejected codec extradata\n");
        AudioConverterDispose(p->converter);
        p->converter = NULL;
        return false;
    }

    // AAC/AC-3/E-AC-3/MP3 frames are small; keep enough room for codecs that
    // expose more than one decoded frame from a single compressed packet.
    p->max_samples = 8192;
    return true;
}

static struct mp_decoder *create(struct mp_filter *parent,
                                 struct mp_codec_params *codec,
                                 const char *decoder)
{
    struct mp_filter *da = mp_filter_create(parent, &(const struct mp_filter_info){
        .name = "ad_avfoundation",
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
