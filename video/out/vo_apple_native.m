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
#include <stdlib.h>
#include <string.h>

#include <libavutil/hwcontext.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>
#include <TargetConditionals.h>
#if TARGET_OS_TV
#import <AVKit/AVKit.h>
#import <UIKit/UIKit.h>
#endif

#include "common/common.h"
#include "common/msg.h"
#include "osdep/timer.h"
#include "sub/osd.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "apple_fel.h"
#include "apple_native.h"
#include "apple_shader_chain.h"
#include "options/m_option.h"
#include "vo.h"

#define QUEUE_AHEAD_NS MP_TIME_MS_TO_NS(100)
#define SHADER_QUEUE_AHEAD_NS MP_TIME_MS_TO_NS(250)
#define MAX_DRIFT 0.02

struct apple_opts {
    char **shaders;
    bool dv_output;
};

struct priv {
    struct apple_opts opts;
    struct apple_shader_chain *chain;
    struct apple_fel *fel;
    struct apple_native_sink sink;
    struct mp_hwdec_ctx vt;
    AVSampleBufferDisplayLayer *layer;
    CMTimebaseRef timebase;
    CALayer *overlay;
    struct mp_osd_res osd_res;
    int64_t osd_change_id;
    bool pixel_buffers;
    bool paused;
    bool synced;
    double rate;
    double last_pts;
    int64_t last_target;
    CMFormatDescriptionRef criteria_format;
    float criteria_fps;
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

static void enqueue_shaded(struct vo *vo, struct mp_image *mpi)
{
    struct priv *p = vo->priv;
    if (!p->chain || !apple_shader_chain_ready(p->chain))
        return;
    CVPixelBufferRef input = (CVPixelBufferRef)mpi->planes[3];
    double pts = mpi->pts;
    if (!input || pts == MP_NOPTS_VALUE)
        return;

    AVSampleBufferDisplayLayer *layer = [p->layer retain];
    bool queued = apple_shader_chain_run(p->chain, input, vo->dwidth, vo->dheight,
                                         ^(CVPixelBufferRef output) {
        CMVideoFormatDescriptionRef format = NULL;
        if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, output,
                                                         &format) == noErr)
        {
            CMSampleTimingInfo timing = {
                .duration = kCMTimeInvalid,
                .presentationTimeStamp = CMTimeMake(llrint(pts * 1e6), 1000000),
                .decodeTimeStamp = kCMTimeInvalid,
            };
            CMSampleBufferRef sample = NULL;
            if (CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, output, format,
                                                         &timing, &sample) == noErr)
            {
                [layer enqueueSampleBuffer:sample];
                CFRelease(sample);
            }
            CFRelease(format);
        }
        [layer release];
    });
    if (queued) {
        p->pixel_buffers = true;
    } else {
        [layer release];
        enqueue_pixel_buffer(vo, mpi);
    }
}

static void enqueue_composed(struct vo *vo, struct mp_image *mpi)
{
    struct priv *p = vo->priv;
    CVPixelBufferRef bl = (CVPixelBufferRef)mpi->planes[3];
    struct mp_image *eli = mpi->enhancement_layer;
    CVPixelBufferRef el = eli && eli->imgfmt == IMGFMT_VIDEOTOOLBOX
                          ? (CVPixelBufferRef)eli->planes[3] : NULL;
    double pts = mpi->pts;
    if (!bl || pts == MP_NOPTS_VALUE)
        return;

    AVSampleBufferDisplayLayer *layer = [p->layer retain];
    int ret = apple_fel_compose(p->fel, bl, el, mpi->params.repr.dovi,
                                ^(CVPixelBufferRef output) {
        CMVideoFormatDescriptionRef format = NULL;
        if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, output,
                                                         &format) == noErr)
        {
            CMSampleTimingInfo timing = {
                .duration = kCMTimeInvalid,
                .presentationTimeStamp = CMTimeMake(llrint(pts * 1e6), 1000000),
                .decodeTimeStamp = kCMTimeInvalid,
            };
            CMSampleBufferRef sample = NULL;
            if (CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, output, format,
                                                         &timing, &sample) == noErr)
            {
                [layer enqueueSampleBuffer:sample];
                CFRelease(sample);
            }
            CFRelease(format);
        }
        [layer release];
    });
    if (ret == APPLE_FEL_QUEUED) {
        p->pixel_buffers = true;
        return;
    }
    [layer release];
    if (ret == APPLE_FEL_UNSUPPORTED)
        enqueue_pixel_buffer(vo, mpi);
}

static void set_display_criteria(AVSampleBufferDisplayLayer *layer,
                                 CMFormatDescriptionRef format, float fps)
{
#if TARGET_OS_TV
    if (@available(tvOS 17.0, *)) {
        CALayer *l = layer;
        while (l && ![l.delegate isKindOfClass:[UIView class]])
            l = l.superlayer;
        AVDisplayManager *manager = ((UIView *)l.delegate).window.avDisplayManager;
        if (!manager)
            return;
        AVDisplayCriteria *criteria = nil;
        if (format)
            criteria = [[[AVDisplayCriteria alloc] initWithRefreshRate:fps
                                                    formatDescription:format] autorelease];
        manager.preferredDisplayCriteria = criteria;
    }
#endif
}

static int dv_level(int width, float fps)
{
    int step = fps <= 24.5f ? 0 : fps <= 30.5f ? 1 : fps <= 48.5f ? 2 : 3;
    if (width > 2560)
        return 6 + step;
    if (width > 1280)
        return step < 2 ? 3 + step : 5;
    return step < 1 ? 1 : 2;
}

static CMFormatDescriptionRef create_dv_format(int w, int h, float fps)
{
    int level = dv_level(w, fps);
    uint8_t config[24] = {1, 0};
    config[2] = 8 << 1 | level >> 5;
    config[3] = (level & 31) << 3 | 1 << 2 | 1;
    config[4] = 1 << 4;
    NSDictionary *ext = @{
        (id)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms:
            @{@"dvvC": [NSData dataWithBytes:config length:sizeof(config)]},
        (id)kCMFormatDescriptionExtension_ColorPrimaries:
            (id)kCMFormatDescriptionColorPrimaries_ITU_R_2020,
        (id)kCMFormatDescriptionExtension_TransferFunction:
            (id)kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ,
        (id)kCMFormatDescriptionExtension_YCbCrMatrix:
            (id)kCMFormatDescriptionYCbCrMatrix_ITU_R_2020,
    };
    CMFormatDescriptionRef format = NULL;
    if (CMVideoFormatDescriptionCreate(kCFAllocatorDefault, 'dvh1', w, h,
                                       (CFDictionaryRef)ext, &format) != noErr)
        return NULL;
    return format;
}

static void update_display_criteria(struct vo *vo, struct mp_image *mpi)
{
    struct priv *p = vo->priv;
    if (mpi->nominal_fps <= 0)
        return;
    CMFormatDescriptionRef format = NULL;
    CMFormatDescriptionRef owned = NULL;
    if (p->opts.dv_output && p->fel && mpi->imgfmt == IMGFMT_VIDEOTOOLBOX &&
        mpi->params.repr.dovi)
    {
        if (p->criteria_format && mpi->nominal_fps == p->criteria_fps)
            return;
        format = owned = create_dv_format(mpi->w, mpi->h, mpi->nominal_fps);
    } else if (mpi->imgfmt == IMGFMT_APPLE_NATIVE) {
        format = (CMFormatDescriptionRef)mpi->planes[3];
        if (format == p->criteria_format && mpi->nominal_fps == p->criteria_fps)
            return;
    }
    if (!format)
        return;
    if (p->criteria_format)
        CFRelease(p->criteria_format);
    p->criteria_format = (CMFormatDescriptionRef)CFRetain(format);
    p->criteria_fps = mpi->nominal_fps;
    MP_VERBOSE(vo, "Display criteria: %.3f fps\n", p->criteria_fps);

    AVSampleBufferDisplayLayer *layer = [p->layer retain];
    CFRetain(format);
    float fps = p->criteria_fps;
    on_main(^{
        set_display_criteria(layer, format, fps);
        CFRelease(format);
        [layer release];
    });
    if (owned)
        CFRelease(owned);
}

static void update_size(struct vo *vo)
{
    struct priv *p = vo->priv;
    CGSize size = p->layer.bounds.size;
    CGFloat scale = p->layer.contentsScale > 0 ? p->layer.contentsScale : 1;
    vo->dwidth = MPMAX(1, lrint(size.width * scale));
    vo->dheight = MPMAX(1, lrint(size.height * scale));

    struct mp_rect src, dst;
    vo_get_src_dst_rects(vo, &src, &dst, &p->osd_res);
}

static void free_bitmap(void *info, const void *data, size_t size)
{
    free((void *)data);
}

static CGImageRef create_bitmap_image(struct sub_bitmap *b)
{
    size_t stride = (size_t)b->w * 4;
    uint8_t *data = malloc(stride * b->h);
    if (!data)
        return NULL;
    for (int y = 0; y < b->h; y++)
        memcpy(data + y * stride, (uint8_t *)b->bitmap + y * b->stride, stride);

    CGDataProviderRef provider =
        CGDataProviderCreateWithData(NULL, data, stride * b->h, free_bitmap);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image = CGImageCreate(b->w, b->h, 8, 32, stride, cs,
                                     kCGBitmapByteOrder32Little |
                                     kCGImageAlphaPremultipliedFirst,
                                     provider, NULL, false,
                                     kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(provider);
    return image;
}

static void draw_osd(struct vo *vo, double pts, int64_t target)
{
    struct priv *p = vo->priv;
    if (p->osd_res.w <= 0 || p->osd_res.h <= 0)
        return;

    static const bool formats[SUBBITMAP_COUNT] = {[SUBBITMAP_BGRA] = true};
    struct sub_bitmap_list *list = osd_render(vo->osd, p->osd_res, pts, 0, formats);
    if (list->change_id == p->osd_change_id) {
        talloc_free(list);
        return;
    }
    p->osd_change_id = list->change_id;

    CGRect bounds = p->layer.bounds;
    CGFloat scale = bounds.size.width / p->osd_res.w;
    NSMutableArray *layers = [[NSMutableArray alloc] init];
    for (int n = 0; n < list->num_items; n++) {
        struct sub_bitmaps *imgs = list->items[n];
        for (int i = 0; i < imgs->num_parts; i++) {
            struct sub_bitmap *b = &imgs->parts[i];
            if (b->w <= 0 || b->h <= 0 || b->dw <= 0 || b->dh <= 0)
                continue;
            CGImageRef image = create_bitmap_image(b);
            if (!image)
                continue;
            CALayer *layer = [[CALayer alloc] init];
            layer.frame = CGRectMake(b->x * scale, b->y * scale,
                                     b->dw * scale, b->dh * scale);
            layer.contents = (id)image;
            layer.contentsGravity = kCAGravityResize;
            [layers addObject:layer];
            [layer release];
            CGImageRelease(image);
        }
    }
    talloc_free(list);

    CALayer *overlay = [p->overlay retain];
    int64_t delay = target > 0 ? MPMAX(0, target - mp_time_ns()) : 0;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay), dispatch_get_main_queue(), ^{
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        overlay.frame = overlay.superlayer.bounds;
        overlay.sublayers = layers;
        [CATransaction commit];
        [layers release];
        [overlay release];
    });
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;
    struct mp_image *mpi = frame->current;
    if (!mpi)
        return true;

    CGSize size = p->layer.bounds.size;
    CGFloat scale = p->layer.contentsScale > 0 ? p->layer.contentsScale : 1;
    if (lrint(size.width * scale) != vo->dwidth ||
        lrint(size.height * scale) != vo->dheight)
    {
        update_size(vo);
        p->osd_change_id = -1;
        vo_event(vo, VO_EVENT_RESIZE);
    }

    @autoreleasepool {
        draw_osd(vo, mpi->pts, frame->redraw ? 0 : frame->pts);
    }
    if (frame->redraw || frame->repeat)
        return true;

    @autoreleasepool {
        if (mpi->imgfmt == IMGFMT_VIDEOTOOLBOX) {
            if (p->fel && mpi->params.repr.dovi)
                enqueue_composed(vo, mpi);
            else if (p->chain)
                enqueue_shaded(vo, mpi);
            else
                enqueue_pixel_buffer(vo, mpi);
        }
        update_display_criteria(vo, mpi);
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
    struct priv *p = vo->priv;
    update_size(vo);
    p->osd_change_id = -1;
    return 0;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;

    switch (request) {
    case VOCTRL_SET_PANSCAN:
        update_size(vo);
        return VO_TRUE;
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

    p->overlay = [[CALayer alloc] init];
    p->overlay.zPosition = 1;

    AVSampleBufferDisplayLayer *layer = [p->layer retain];
    CALayer *overlay = [p->overlay retain];
    CMTimebaseRef timebase = (CMTimebaseRef)CFRetain(p->timebase);
    on_main(^{
        layer.videoGravity = AVLayerVideoGravityResizeAspect;
        layer.controlTimebase = timebase;
        overlay.frame = layer.bounds;
        [layer addSublayer:overlay];
        [overlay release];
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

    char **shaders = p->opts.shaders;
    if (shaders && shaders[0] && p->vt.av_device_ref)
        p->chain = apple_shader_chain_create(vo->global, vo->log, shaders);

    if (p->vt.av_device_ref)
        p->fel = apple_fel_create(vo->log, p->opts.dv_output);

    vo->hwdec_devs = hwdec_devices_create();
    if (!p->chain)
        hwdec_devices_add(vo->hwdec_devs, &p->sink.hwctx);
    if (p->vt.av_device_ref)
        hwdec_devices_add(vo->hwdec_devs, &p->vt);

    vo_set_queue_params(vo, p->chain || p->fel ? SHADER_QUEUE_AHEAD_NS : QUEUE_AHEAD_NS, 1);
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;

    apple_shader_chain_destroy(p->chain);
    apple_fel_destroy(p->fel);
    if (vo->hwdec_devs) {
        if (!p->chain)
            hwdec_devices_remove(vo->hwdec_devs, &p->sink.hwctx);
        hwdec_devices_remove(vo->hwdec_devs, &p->vt);
        hwdec_devices_destroy(vo->hwdec_devs);
    }
    av_buffer_unref(&p->vt.av_device_ref);

    AVSampleBufferDisplayLayer *layer = p->layer;
    CALayer *overlay = p->overlay;
    bool criteria = p->criteria_format;
    on_main(^{
        if (criteria)
            set_display_criteria(layer, NULL, 0);
        [overlay removeFromSuperlayer];
        [overlay release];
        [layer flushAndRemoveImage];
        layer.controlTimebase = nil;
        [layer release];
    });
    CFRelease(p->timebase);
    if (p->criteria_format)
        CFRelease(p->criteria_format);
}

#define OPT_BASE_STRUCT struct priv

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
    .options = (const m_option_t[]) {
        {"shaders", OPT_PATHLIST(opts.shaders), .flags = M_OPT_FILE},
        {"dv-output", OPT_BOOL(opts.dv_output)},
        {0}
    },
    .options_prefix = "vo-apple-native",
};
