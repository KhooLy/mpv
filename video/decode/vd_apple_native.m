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

#include <libavcodec/avcodec.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mastering_display_metadata.h>
#include <libplacebo/utils/libav.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include "common/common.h"
#include "common/msg.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "filters/f_decoder_wrapper.h"
#include "filters/filter_internal.h"
#include "video/mp_image.h"
#include "video/out/apple_native.h"

#define REORDER_DEPTH 8

struct priv {
    struct mp_decoder public;
    struct mp_log *log;
    struct mp_codec_params *codec;
    AVSampleBufferDisplayLayer *layer;
    CMTimebaseRef timebase;
    CMVideoFormatDescriptionRef format;
    struct mp_image_params params;
    double pending[REORDER_DEPTH + 1];
    int num_pending;
    bool need_keyframe;
    bool eof;
};

static void put_be16(uint8_t *d, double v)
{
    unsigned n = MPCLAMP(lrint(v), 0, UINT16_MAX);
    d[0] = n >> 8;
    d[1] = n;
}

static void put_be32(uint8_t *d, double v)
{
    uint32_t n = MPCLAMP(llrint(v), 0, UINT32_MAX);
    d[0] = n >> 24;
    d[1] = n >> 16;
    d[2] = n >> 8;
    d[3] = n;
}

static CMTime to_cmtime(double s)
{
    return CMTimeMake(llrint(s * 1e6), 1000000);
}

static bool has_iso_config(const uint8_t *d, int size)
{
    if (!d || size < 4)
        return false;
    return !(d[0] == 0 && d[1] == 0 && (d[2] == 1 || (d[2] == 0 && d[3] == 1)));
}

static const AVDOVIDecoderConfigurationRecord *get_dovi(const struct mp_codec_params *c)
{
    const AVCodecParameters *par = c->lav_codecpar;
    if (!par)
        return NULL;
    const AVPacketSideData *sd = av_packet_side_data_get(
        par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF);
    return sd ? (const AVDOVIDecoderConfigurationRecord *)sd->data : NULL;
}

static NSData *dovi_atom(const AVDOVIDecoderConfigurationRecord *dovi)
{
    uint8_t b[24] = {0};
    b[0] = dovi->dv_version_major;
    b[1] = dovi->dv_version_minor;
    b[2] = (dovi->dv_profile << 1) | (dovi->dv_level >> 5);
    b[3] = ((dovi->dv_level & 0x1f) << 3) | (dovi->rpu_present_flag << 2) |
           (dovi->el_present_flag << 1) | dovi->bl_present_flag;
    b[4] = dovi->dv_bl_signal_compatibility_id << 4;
    return [NSData dataWithBytes:b length:sizeof(b)];
}

static NSData *mastering_display(const struct pl_hdr_metadata *hdr)
{
    if (hdr->max_luma <= 0 || hdr->prim.red.x <= 0)
        return nil;
    const struct pl_cie_xy *xy[] = {
        &hdr->prim.green, &hdr->prim.blue, &hdr->prim.red, &hdr->prim.white,
    };
    uint8_t b[24];
    for (int i = 0; i < 4; i++) {
        put_be16(b + i * 4, xy[i]->x * 50000);
        put_be16(b + i * 4 + 2, xy[i]->y * 50000);
    }
    put_be32(b + 16, hdr->max_luma * 10000);
    put_be32(b + 20, hdr->min_luma * 10000);
    return [NSData dataWithBytes:b length:sizeof(b)];
}

static NSData *content_light(const struct pl_hdr_metadata *hdr)
{
    if (hdr->max_cll <= 0 && hdr->max_fall <= 0)
        return nil;
    uint8_t b[4];
    put_be16(b, hdr->max_cll);
    put_be16(b + 2, hdr->max_fall);
    return [NSData dataWithBytes:b length:sizeof(b)];
}

static void fill_from_codecpar(struct pl_color_space *color, struct pl_color_repr *repr,
                               const AVCodecParameters *par)
{
    if (!par)
        return;
    if (!color->primaries)
        color->primaries = pl_primaries_from_av(par->color_primaries);
    if (!color->transfer)
        color->transfer = pl_transfer_from_av(par->color_trc);
    if (!repr->sys)
        repr->sys = pl_system_from_av(par->color_space);
    if (!repr->levels)
        repr->levels = pl_levels_from_av(par->color_range);

    const AVPacketSideData *sd = par->coded_side_data;
    int n = par->nb_coded_side_data;
    const AVPacketSideData *mdm =
        av_packet_side_data_get(sd, n, AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
    const AVPacketSideData *clm =
        av_packet_side_data_get(sd, n, AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
    if (!pl_hdr_metadata_equal(&color->hdr, &pl_hdr_metadata_empty))
        return;
    pl_map_hdr_metadata(&color->hdr, &(struct pl_av_hdr_metadata){
        .mdm = mdm ? (const AVMasteringDisplayMetadata *)mdm->data : NULL,
        .clm = clm ? (const AVContentLightMetadata *)clm->data : NULL,
    });
}

static void fill_from_parameter_sets(struct pl_color_space *color,
                                     struct pl_color_repr *repr,
                                     CMVideoCodecType type,
                                     const uint8_t *d, int size)
{
    const uint8_t *sets[16];
    size_t sizes[16];
    int count = 0, nal_len, pos;

    if (type == kCMVideoCodecType_H264) {
        if (size < 7)
            return;
        nal_len = (d[4] & 3) + 1;
        pos = 5;
        for (int list = 0; list < 2 && pos < size; list++) {
            int num = list ? d[pos] : d[pos] & 0x1f;
            pos++;
            for (int i = 0; i < num && pos + 2 <= size && count < 16; i++) {
                int len = AV_RB16(d + pos);
                if (pos + 2 + len > size)
                    return;
                sets[count] = d + pos + 2;
                sizes[count++] = len;
                pos += 2 + len;
            }
        }
    } else {
        if (size < 23)
            return;
        nal_len = (d[21] & 3) + 1;
        int arrays = d[22];
        pos = 23;
        for (int a = 0; a < arrays && pos + 3 <= size; a++) {
            int nal_type = d[pos] & 0x3f;
            int num = AV_RB16(d + pos + 1);
            pos += 3;
            for (int i = 0; i < num && pos + 2 <= size; i++) {
                int len = AV_RB16(d + pos);
                if (pos + 2 + len > size)
                    return;
                if (nal_type >= 32 && nal_type <= 34 && count < 16) {
                    sets[count] = d + pos + 2;
                    sizes[count++] = len;
                }
                pos += 2 + len;
            }
        }
    }
    if (!count)
        return;

    CMFormatDescriptionRef fd = NULL;
    OSStatus err = type == kCMVideoCodecType_H264 ?
        CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, count, sets, sizes,
                                                            nal_len, &fd) :
        CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, count, sets, sizes,
                                                            nal_len, NULL, &fd);
    if (err != noErr)
        return;

    CFStringRef prim = CMFormatDescriptionGetExtension(fd,
        kCMFormatDescriptionExtension_ColorPrimaries);
    CFStringRef trc = CMFormatDescriptionGetExtension(fd,
        kCMFormatDescriptionExtension_TransferFunction);
    CFStringRef matrix = CMFormatDescriptionGetExtension(fd,
        kCMFormatDescriptionExtension_YCbCrMatrix);
    CFBooleanRef full = CMFormatDescriptionGetExtension(fd,
        kCMFormatDescriptionExtension_FullRangeVideo);
    if (!color->primaries && prim)
        color->primaries = pl_primaries_from_av(CVColorPrimariesGetIntegerCodePointForString(prim));
    if (!color->transfer && trc)
        color->transfer = pl_transfer_from_av(CVTransferFunctionGetIntegerCodePointForString(trc));
    if (!repr->sys && matrix)
        repr->sys = pl_system_from_av(CVYCbCrMatrixGetIntegerCodePointForString(matrix));
    if (!repr->levels && full)
        repr->levels = CFBooleanGetValue(full) ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
    CFRelease(fd);
}

static bool init_format(struct priv *p)
{
    struct mp_codec_params *c = p->codec;
    const char *codec = c->codec;
    const uint8_t *extradata = c->extradata;
    int extradata_size = c->extradata_size;
    if (!extradata_size && c->lav_codecpar) {
        extradata = c->lav_codecpar->extradata;
        extradata_size = c->lav_codecpar->extradata_size;
    }
    if (!codec || !has_iso_config(extradata, extradata_size)) {
        MP_VERBOSE(p, "No ISO codec configuration record, not using native decoding\n");
        return false;
    }

    CMVideoCodecType type;
    NSString *atom;
    if (strcmp(codec, "h264") == 0) {
        type = kCMVideoCodecType_H264;
        atom = @"avcC";
    } else if (strcmp(codec, "hevc") == 0) {
        type = kCMVideoCodecType_HEVC;
        atom = @"hvcC";
    } else if (strcmp(codec, "av1") == 0) {
        type = 'av01';
        atom = @"av1C";
    } else {
        return false;
    }

    if (!VTIsHardwareDecodeSupported(type)) {
        MP_VERBOSE(p, "No hardware decoder for %s\n", codec);
        return false;
    }

    NSMutableDictionary *atoms = [NSMutableDictionary dictionary];
    atoms[atom] = [NSData dataWithBytes:extradata length:extradata_size];

    const AVDOVIDecoderConfigurationRecord *dovi = get_dovi(c);
    if (dovi && type == kCMVideoCodecType_HEVC) {
        bool native = (dovi->dv_profile == 5 || dovi->dv_profile == 8) &&
                      VTIsHardwareDecodeSupported('dvh1');
        if (native) {
            type = 'dvh1';
            atoms[dovi->dv_profile > 7 ? @"dvvC" : @"dvcC"] = dovi_atom(dovi);
            MP_VERBOSE(p, "Dolby Vision profile %d.%d\n", dovi->dv_profile,
                       dovi->dv_level);
        } else if (dovi->dv_profile == 5) {
            MP_WARN(p, "Dolby Vision profile 5 is not supported natively here\n");
            return false;
        } else {
            MP_VERBOSE(p, "Dolby Vision profile %d, using the base layer\n",
                       dovi->dv_profile);
        }
    }

    struct pl_color_space color = c->color;
    struct pl_color_repr repr = c->repr;
    fill_from_codecpar(&color, &repr, c->lav_codecpar);
    if (type == kCMVideoCodecType_H264 || type == kCMVideoCodecType_HEVC || type == 'dvh1') {
        fill_from_parameter_sets(&color, &repr, type == kCMVideoCodecType_H264 ?
                                 type : kCMVideoCodecType_HEVC,
                                 extradata, extradata_size);
    }

    NSMutableDictionary *ext = [NSMutableDictionary dictionary];
    ext[(id)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms] = atoms;

    CFStringRef prim = CVColorPrimariesGetStringForIntegerCodePoint(
        pl_primaries_to_av(color.primaries));
    CFStringRef trc = CVTransferFunctionGetStringForIntegerCodePoint(
        pl_transfer_to_av(color.transfer));
    CFStringRef matrix = CVYCbCrMatrixGetStringForIntegerCodePoint(
        pl_system_to_av(repr.sys));
    if (prim)
        ext[(id)kCMFormatDescriptionExtension_ColorPrimaries] = (id)prim;
    if (trc)
        ext[(id)kCMFormatDescriptionExtension_TransferFunction] = (id)trc;
    if (matrix)
        ext[(id)kCMFormatDescriptionExtension_YCbCrMatrix] = (id)matrix;
    if (repr.levels == PL_COLOR_LEVELS_FULL)
        ext[(id)kCMFormatDescriptionExtension_FullRangeVideo] = @YES;

    if (c->par_w > 0 && c->par_h > 0) {
        ext[(id)kCMFormatDescriptionExtension_PixelAspectRatio] = @{
            (id)kCMFormatDescriptionKey_PixelAspectRatioHorizontalSpacing: @(c->par_w),
            (id)kCMFormatDescriptionKey_PixelAspectRatioVerticalSpacing: @(c->par_h),
        };
    }

    NSData *mdcv = mastering_display(&color.hdr);
    if (mdcv)
        ext[(id)kCMFormatDescriptionExtension_MasteringDisplayColorVolume] = mdcv;
    NSData *clli = content_light(&color.hdr);
    if (clli)
        ext[(id)kCMFormatDescriptionExtension_ContentLightLevelInfo] = clli;

    int w = c->lav_codecpar && c->lav_codecpar->width ? c->lav_codecpar->width : c->disp_w;
    int h = c->lav_codecpar && c->lav_codecpar->height ? c->lav_codecpar->height : c->disp_h;
    if (w <= 0 || h <= 0)
        return false;

    OSStatus err = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, type, w, h,
                                                  (CFDictionaryRef)ext, &p->format);
    if (err != noErr) {
        MP_ERR(p, "CMVideoFormatDescriptionCreate failed: %d\n", (int)err);
        return false;
    }

    p->params = (struct mp_image_params){
        .imgfmt = IMGFMT_APPLE_NATIVE,
        .w = c->disp_w > 0 ? c->disp_w : w,
        .h = c->disp_h > 0 ? c->disp_h : h,
        .p_w = c->par_w > 0 ? c->par_w : 1,
        .p_h = c->par_h > 0 ? c->par_h : 1,
        .color = color,
        .repr = repr,
        .chroma_location = c->chroma_location,
        .rotate = c->rotate,
    };
    return true;
}

static CMSampleBufferRef create_sample(struct priv *p, struct demux_packet *pkt)
{
    CMBlockBufferRef block = NULL;
    if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, pkt->len,
                                           kCFAllocatorDefault, NULL, 0, pkt->len,
                                           kCMBlockBufferAssureMemoryNowFlag,
                                           &block) != noErr)
        return NULL;
    if (CMBlockBufferReplaceDataBytes(pkt->buffer, block, 0, pkt->len) != noErr) {
        CFRelease(block);
        return NULL;
    }

    CMSampleTimingInfo timing = {
        .duration = pkt->duration > 0 ? to_cmtime(pkt->duration) : kCMTimeInvalid,
        .presentationTimeStamp = pkt->pts != MP_NOPTS_VALUE ? to_cmtime(pkt->pts)
                                                            : kCMTimeInvalid,
        .decodeTimeStamp = pkt->dts != MP_NOPTS_VALUE ? to_cmtime(pkt->dts)
                                                      : kCMTimeInvalid,
    };
    size_t size = pkt->len;
    CMSampleBufferRef sample = NULL;
    OSStatus err = CMSampleBufferCreateReady(kCFAllocatorDefault, block, p->format,
                                             1, 1, &timing, 1, &size, &sample);
    CFRelease(block);
    if (err != noErr) {
        MP_ERR(p, "CMSampleBufferCreateReady failed: %d\n", (int)err);
        return NULL;
    }

    if (!pkt->keyframe) {
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, true);
        CFMutableDictionaryRef dict =
            (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFDictionarySetValue(dict, kCMSampleAttachmentKey_NotSync, kCFBooleanTrue);
    }
    return sample;
}

static void check_layer(struct priv *p)
{
    bool flush = false;
    if (p->layer.status == AVQueuedSampleBufferRenderingStatusFailed) {
        MP_ERR(p, "Display layer failed: %s\n",
               p->layer.error.localizedDescription.UTF8String);
        flush = true;
    }
    if (@available(macOS 11.0, iOS 14.0, tvOS 14.0, *)) {
        if (p->layer.requiresFlushToResumeDecoding)
            flush = true;
    }
    if (flush) {
        [p->layer flush];
        p->need_keyframe = true;
    }
}

static void enqueue(struct priv *p, struct demux_packet *pkt)
{
    check_layer(p);
    if (p->need_keyframe && !pkt->keyframe)
        return;
    p->need_keyframe = false;

    CMSampleBufferRef sample = create_sample(p, pkt);
    if (!sample)
        return;
    [p->layer enqueueSampleBuffer:sample];
    CFRelease(sample);

    double pts = pkt->pts != MP_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (pts != MP_NOPTS_VALUE)
        p->pending[p->num_pending++] = pts;
}

static void noop_free(void *arg)
{
}

static void emit(struct mp_filter *f)
{
    struct priv *p = f->priv;

    int best = 0;
    for (int n = 1; n < p->num_pending; n++) {
        if (p->pending[n] < p->pending[best])
            best = n;
    }
    double pts = p->pending[best];
    p->pending[best] = p->pending[--p->num_pending];

    struct mp_image t = {0};
    mp_image_set_params(&t, &p->params);
    struct mp_image *mpi = mp_image_new_custom_ref(&t, NULL, noop_free);
    MP_HANDLE_OOM(mpi);
    mpi->pts = pts;
    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, mpi));
}

static void process(struct mp_filter *f)
{
    struct priv *p = f->priv;

    if (!mp_pin_in_needs_data(f->ppins[1]))
        return;

    if (p->num_pending > REORDER_DEPTH || (p->eof && p->num_pending)) {
        emit(f);
        return;
    }

    if (p->eof) {
        p->eof = false;
        mp_pin_in_write(f->ppins[1], MP_EOF_FRAME);
        return;
    }

    struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
    if (frame.type == MP_FRAME_NONE)
        return;

    if (frame.type == MP_FRAME_EOF) {
        p->eof = true;
    } else if (frame.type == MP_FRAME_PACKET) {
        @autoreleasepool {
            enqueue(p, frame.data);
        }
        mp_frame_unref(&frame);
    } else {
        MP_ERR(p, "unexpected frame type\n");
        mp_frame_unref(&frame);
    }
    mp_filter_internal_mark_progress(f);
}

static void reset(struct mp_filter *f)
{
    struct priv *p = f->priv;

    p->num_pending = 0;
    p->eof = false;
    p->need_keyframe = true;
    [p->layer flush];
    CMTimebaseSetRate(p->timebase, 0);
    CMTimebaseSetTime(p->timebase, CMTimeMake(-1000000, 1));
}

static void destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;

    [p->layer flush];
    [p->layer release];
    if (p->timebase)
        CFRelease(p->timebase);
    if (p->format)
        CFRelease(p->format);
}

static const struct mp_filter_info vd_apple_native_filter = {
    .name = "vd_apple_native",
    .priv_size = sizeof(struct priv),
    .process = process,
    .reset = reset,
    .destroy = destroy,
};

static struct mp_decoder *create(struct mp_filter *parent,
                                 struct mp_codec_params *codec,
                                 const char *decoder)
{
    struct mp_stream_info *info = mp_filter_find_stream_info(parent);
    if (!info || !info->hwdec_devs)
        return NULL;
    struct mp_hwdec_ctx *ctx = hwdec_devices_get_by_imgfmt_and_type(
        info->hwdec_devs, IMGFMT_APPLE_NATIVE, AV_HWDEVICE_TYPE_NONE);
    if (!ctx)
        return NULL;
    struct apple_native_sink *sink = (struct apple_native_sink *)ctx;

    struct mp_filter *f = mp_filter_create(parent, &vd_apple_native_filter);
    if (!f)
        return NULL;
    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");
    f->log = mp_log_new(f, parent->log, NULL);

    struct priv *p = f->priv;
    p->log = f->log;
    p->codec = codec;
    p->public.f = f;
    p->need_keyframe = true;
    p->layer = [(AVSampleBufferDisplayLayer *)sink->layer retain];
    p->timebase = (CMTimebaseRef)CFRetain(sink->timebase);

    bool ok;
    @autoreleasepool {
        ok = init_format(p);
    }
    if (!ok) {
        talloc_free(f);
        return NULL;
    }

    return &p->public;
}

const struct mp_decoder_fns vd_apple_native = {
    .create = create,
};
