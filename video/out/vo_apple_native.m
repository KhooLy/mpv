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
 */

#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_videotoolbox.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include "common/common.h"
#include "common/msg.h"
#include "osdep/timer.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "vo.h"

/*
 * Embedded Apple native output.
 *
 * WinID contains an unretained AVSampleBufferDisplayLayer pointer supplied by
 * the host. The host must keep the layer alive until the mpv VO is destroyed.
 * The VO retains it while active as an additional safety measure.
 *
 * The decoder still belongs to mpv/FFmpeg. With hwdec=videotoolbox, decoded
 * CVPixelBuffers stay in the VideoToolbox hardware path and are handed to the
 * Apple sample-buffer display layer without a readback or libplacebo pass.
 */

struct priv {
    struct mp_log *log;
    AVSampleBufferDisplayLayer *layer;
    CMTimebaseRef timebase;
    struct mp_image *next_image;
    int64_t next_pts;
    struct mp_hwdec_ctx hwctx;
};

static AVBufferRef *create_videotoolbox_device_ref(void)
{
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
    if (!device_ref)
        return NULL;

    if (av_hwdevice_ctx_init(device_ref) < 0) {
        av_buffer_unref(&device_ref);
        return NULL;
    }

    return device_ref;
}

static bool create_sample_buffer(struct priv *p, struct mp_image *mpi,
                                 int64_t pts, CMSampleBufferRef *out)
{
    if (mpi->imgfmt != IMGFMT_VIDEOTOOLBOX || !mpi->planes[3])
        return false;

    CVPixelBufferRef pixel_buffer = (CVPixelBufferRef)mpi->planes[3];
    CMVideoFormatDescriptionRef format = NULL;
    CMSampleBufferRef sample = NULL;

    if (CMVideoFormatDescriptionCreateForImageBuffer(
            kCFAllocatorDefault, pixel_buffer, &format) != noErr)
        goto error;

    CMSampleTimingInfo timing = {
        .duration = kCMTimeInvalid,
        .presentationTimeStamp = CMTimeMake(pts > 0 ? pts : mp_time_ns(),
                                            1000000000),
        .decodeTimeStamp = kCMTimeInvalid,
    };

    if (CMSampleBufferCreateForImageBuffer(
            kCFAllocatorDefault, pixel_buffer, true, NULL, NULL, format,
            &timing, &sample) != noErr)
        goto error;

    // Keep HDR10/HLG/Dolby Vision color attachments associated with the
    // sample buffer consumed by AVSampleBufferDisplayLayer. CoreVideo and
    // CoreMedia use different attachment bearer types, so copy the dictionary
    // through the CoreMedia API instead of calling CVBufferPropagateAttachments
    // on a CMSampleBuffer.
    CFDictionaryRef attachments = CVBufferGetAttachments(
        pixel_buffer, kCVAttachmentMode_ShouldPropagate);
    if (attachments)
        CMSetAttachments(sample, attachments, kCMAttachmentMode_ShouldPropagate);
    *out = sample;
    CFRelease(format);
    return true;

error:
    if (sample)
        CFRelease(sample);
    if (format)
        CFRelease(format);
    MP_VERBOSE(p->log, "Could not create CMSampleBuffer from CVPixelBuffer\n");
    return false;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    p->log = vo->log;

    if (vo->opts->WinID <= 0) {
        MP_FATAL(vo, "--wid must contain an AVSampleBufferDisplayLayer pointer\n");
        return -1;
    }

    p->layer = (AVSampleBufferDisplayLayer *)(uintptr_t)vo->opts->WinID;
    [p->layer retain];

    if (CMTimebaseCreateWithMasterClock(
            kCFAllocatorDefault, CMClockGetHostTimeClock(), &p->timebase) != noErr) {
        MP_FATAL(vo, "Could not create native presentation timebase\n");
        goto error;
    }

    CMTimebaseSetTime(p->timebase,
                      CMClockGetTime(CMClockGetHostTimeClock()));
    CMTimebaseSetRate(p->timebase, 1.0);
    [p->layer setControlTimebase:p->timebase];

    vo->hwdec_devs = hwdec_devices_create();
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = "apple_native",
        .av_device_ref = create_videotoolbox_device_ref(),
        .hw_imgfmt = IMGFMT_VIDEOTOOLBOX,
    };
    if (!p->hwctx.av_device_ref) {
        MP_FATAL(vo, "Could not create VideoToolbox hardware device\n");
        goto error;
    }

    hwdec_devices_add(vo->hwdec_devs, &p->hwctx);
    return 0;

error:
    av_buffer_unref(&p->hwctx.av_device_ref);
    if (p->timebase)
        CFRelease(p->timebase);
    if (p->layer)
        [p->layer release];
    p->timebase = NULL;
    p->layer = nil;
    return -1;
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

    mp_image_t *mpi = NULL;
    if (!frame->redraw && !frame->repeat)
        mpi = mp_image_new_ref(frame->current);

    mp_image_unrefp(&p->next_image);
    p->next_image = mpi;
    p->next_pts = mpi ? frame->pts : 0;
    return true;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!p->next_image)
        return;

    CMSampleBufferRef sample = NULL;
    if (create_sample_buffer(p, p->next_image, p->next_pts, &sample)) {
        if ([p->layer isReadyForMoreMediaData]) {
            [p->layer enqueueSampleBuffer:sample];
        } else {
            MP_WARN(vo, "Apple native display layer is not ready; dropping frame\n");
        }
        CFRelease(sample);
    }

    mp_image_unrefp(&p->next_image);
    p->next_pts = 0;
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT_VIDEOTOOLBOX;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;

    switch (request) {
    case VOCTRL_RESET:
        [p->layer flush];
        return VO_TRUE;
    case VOCTRL_PAUSE:
        CMTimebaseSetRate(p->timebase, 0.0);
        return VO_TRUE;
    case VOCTRL_RESUME:
        CMTimebaseSetRate(p->timebase, 1.0);
        return VO_TRUE;
    default:
        return VO_NOTIMPL;
    }
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    // The CVPixelBuffer and its format description carry the source color
    // properties. No libplacebo conversion is performed in this VO.
    return params->imgfmt == IMGFMT_VIDEOTOOLBOX ? 0 : -1;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;

    mp_image_unrefp(&p->next_image);
    if (p->layer)
        [p->layer flushAndRemoveImage];
    if (p->timebase)
        CFRelease(p->timebase);

    if (vo->hwdec_devs)
        hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
    if (p->layer)
        [p->layer release];
}

const struct vo_driver video_out_apple_native = {
    .description = "Apple native VideoToolbox/CoreMedia output",
    .name = "apple_native",
    .caps = VO_CAP_NORETAIN,
    .preinit = preinit,
    .query_format = query_format,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .reconfig = reconfig,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};
