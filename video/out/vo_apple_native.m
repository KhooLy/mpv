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

#include <libavutil/hwcontext.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include "common/common.h"
#include "common/msg.h"
#include "osdep/timer.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "apple_native.h"
#include "vo.h"

#define QUEUE_AHEAD_NS MP_TIME_MS_TO_NS(100)
#define MAX_DRIFT 0.02

struct priv {
    struct apple_native_sink sink;
    struct mp_hwdec_ctx vt;
    AVSampleBufferDisplayLayer *layer;
    CMTimebaseRef timebase;
    bool pixel_buffers;
    bool paused;
    bool synced;
    double rate;
    double last_pts;
    int64_t last_target;
};

static void on_main(void (^block)(void))
{
    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

static CMTime host_time_at(int64_t mp_ns)
{
    CMTime now = CMClockGetTime(CMClockGetHostTimeClock());
    return CMTimeAdd(now, CMTimeMake(mp_ns - mp_time_ns(), 1000000000));
}

static void sync_clock(struct vo *vo, double pts, int64_t target)
{
    struct priv *p = vo->priv;

    if (target <= 0)
        target = mp_time_ns();

    if (p->last_target && target > p->last_target && pts > p->last_pts) {
        double rate = (pts - p->last_pts) /
                      ((target - p->last_target) / 1e9);
        if (rate > 0.05 && rate < 16 && fabs(rate - p->rate) > 0.02)
            p->rate = rate;
    }
    p->last_pts = pts;
    p->last_target = target;

    double rate = p->paused ? 0 : p->rate;
    double expected = pts - (target - mp_time_ns()) / 1e9 * rate;
    double current = CMTimeGetSeconds(CMTimebaseGetTime(p->timebase));
    if (p->synced && CMTimebaseGetRate(p->timebase) == rate &&
        fabs(current - expected) < MAX_DRIFT)
        return;

    CMTimebaseSetRateAndAnchorTime(p->timebase, rate,
                                   CMTimeMake(llrint(pts * 1e6), 1000000),
                                   host_time_at(target));
    p->synced = true;
}

static void enqueue_pixel_buffer(struct vo *vo, struct mp_image *mpi)
{
    struct priv *p = vo->priv;
    CVPixelBufferRef pixbuf = (CVPixelBufferRef)mpi->planes[3];
    if (!pixbuf || mpi->pts == MP_NOPTS_VALUE)
        return;

    CMVideoFormatDescriptionRef format = NULL;
    if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixbuf,
                                                     &format) != noErr)
        return;

    CMSampleTimingInfo timing = {
        .duration = kCMTimeInvalid,
        .presentationTimeStamp = CMTimeMake(llrint(mpi->pts * 1e6), 1000000),
        .decodeTimeStamp = kCMTimeInvalid,
    };
    CMSampleBufferRef sample = NULL;
    if (CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, pixbuf, format,
                                                 &timing, &sample) == noErr)
    {
        [p->layer enqueueSampleBuffer:sample];
        p->pixel_buffers = true;
        CFRelease(sample);
    } else {
        MP_VERBOSE(vo, "Could not wrap CVPixelBuffer in a sample buffer\n");
    }
    CFRelease(format);
}

static void update_size(struct vo *vo)
{
    struct priv *p = vo->priv;
    CGSize size = p->layer.bounds.size;
    CGFloat scale = p->layer.contentsScale > 0 ? p->layer.contentsScale : 1;
    vo->dwidth = MPMAX(1, lrint(size.width * scale));
    vo->dheight = MPMAX(1, lrint(size.height * scale));
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct mp_image *mpi = frame->current;
    if (!mpi || frame->redraw || frame->repeat)
        return true;

    @autoreleasepool {
        if (mpi->imgfmt == IMGFMT_VIDEOTOOLBOX)
            enqueue_pixel_buffer(vo, mpi);
        if (mpi->pts != MP_NOPTS_VALUE)
            sync_clock(vo, mpi->pts, frame->pts);
    }
    return true;
}

static void flip_page(struct vo *vo)
{
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT_APPLE_NATIVE || format == IMGFMT_VIDEOTOOLBOX;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    update_size(vo);
    return 0;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;

    switch (request) {
    case VOCTRL_RESET:
        if (p->pixel_buffers)
            [p->layer flush];
        CMTimebaseSetRate(p->timebase, 0);
        p->synced = false;
        p->last_target = 0;
        return VO_TRUE;
    case VOCTRL_PAUSE:
        p->paused = true;
        CMTimebaseSetRate(p->timebase, 0);
        return VO_TRUE;
    case VOCTRL_RESUME:
        p->paused = false;
        p->synced = false;
        p->last_target = 0;
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

static AVBufferRef *create_videotoolbox_device(void)
{
    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
    if (ref && av_hwdevice_ctx_init(ref) < 0)
        av_buffer_unref(&ref);
    return ref;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;

    id object = (id)(intptr_t)vo->opts->WinID;
    if (vo->opts->WinID <= 0 ||
        ![object isKindOfClass:[AVSampleBufferDisplayLayer class]])
    {
        MP_ERR(vo, "--wid must be an AVSampleBufferDisplayLayer\n");
        return -1;
    }

    if (CMTimebaseCreateWithSourceClock(kCFAllocatorDefault,
                                        CMClockGetHostTimeClock(),
                                        &p->timebase) != noErr)
    {
        MP_FATAL(vo, "Could not create presentation timebase\n");
        return -1;
    }
    CMTimebaseSetRate(p->timebase, 0);
    p->rate = 1;
    p->layer = [object retain];

    AVSampleBufferDisplayLayer *layer = [p->layer retain];
    CMTimebaseRef timebase = (CMTimebaseRef)CFRetain(p->timebase);
    on_main(^{
        layer.videoGravity = AVLayerVideoGravityResizeAspect;
        layer.controlTimebase = timebase;
        CFRelease(timebase);
        [layer release];
    });

    p->sink = (struct apple_native_sink){
        .hwctx = {
            .driver_name = "apple_native",
            .hw_imgfmt = IMGFMT_APPLE_NATIVE,
        },
        .layer = p->layer,
        .timebase = p->timebase,
    };
    p->vt = (struct mp_hwdec_ctx){
        .driver_name = "videotoolbox",
        .av_device_ref = create_videotoolbox_device(),
        .hw_imgfmt = IMGFMT_VIDEOTOOLBOX,
    };

    vo->hwdec_devs = hwdec_devices_create();
    hwdec_devices_add(vo->hwdec_devs, &p->sink.hwctx);
    if (p->vt.av_device_ref)
        hwdec_devices_add(vo->hwdec_devs, &p->vt);

    vo_set_queue_params(vo, QUEUE_AHEAD_NS, 1);
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;

    if (vo->hwdec_devs) {
        hwdec_devices_remove(vo->hwdec_devs, &p->sink.hwctx);
        hwdec_devices_remove(vo->hwdec_devs, &p->vt);
        hwdec_devices_destroy(vo->hwdec_devs);
    }
    av_buffer_unref(&p->vt.av_device_ref);

    AVSampleBufferDisplayLayer *layer = p->layer;
    on_main(^{
        [layer flushAndRemoveImage];
        layer.controlTimebase = nil;
        [layer release];
    });
    CFRelease(p->timebase);
}

const struct vo_driver video_out_apple_native = {
    .description = "Apple AVSampleBufferDisplayLayer output",
    .name = "apple_native",
    .caps = VO_CAP_NORETAIN | VO_CAP_FRAMEDROP,
    .preinit = preinit,
    .query_format = query_format,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .reconfig = reconfig,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};
