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

#include <dlfcn.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

#include <android/choreographer.h>
#include <android/looper.h>
#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>

#include "common/common.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "vo.h"
#include "video/mp_image.h"
#include "video/hwdec.h"
#include "android_common.h"

#define EARLY_SCHEDULING_THRESHOLD_NS INT64_C(50000000)
#define LATE_THRESHOLD_NS INT64_C(-30000000)
#define MAX_ALLOWED_ADJUSTMENT_NS INT64_C(20000000)
#define VSYNC_OFFSET_PERCENTAGE 80
#define VSYNC_SAMPLE_UPDATE_PERIOD_MS 500
#define UNSET INT64_MIN

struct vsync_sampler {
    mp_thread thread;
    bool running;
    ALooper *looper;
    mp_mutex lock;
    mp_cond ready;
    bool started;
    atomic_bool quit;
    _Atomic int64_t sampled_vsync_ns;
    _Atomic int64_t vsync_duration_ns;
    int64_t last_frame_ns;
    bool measuring;
    void (*post)(AChoreographer *, AChoreographer_frameCallback64, void *);
    void (*post_delayed)(AChoreographer *, AChoreographer_frameCallback64, void *,
                         uint32_t);
    AChoreographer *choreographer;
};

struct release_helper {
    int64_t last_frame_index;
    int64_t last_release_ns;
    int64_t last_pts_us;
    int64_t last_hysteresis_ns;
    int64_t pending_frame_index;
    int64_t pending_release_ns;
    int64_t pending_pts_us;
    int64_t pending_hysteresis_ns;
};

struct priv {
    struct mp_image *next_image;
    int64_t next_image_pts;
    int64_t next_frame_duration_ns;
    struct mp_hwdec_ctx hwctx;

    struct vsync_sampler vsync;
    struct release_helper release;
    int64_t frame_index;
    bool first_frame_rendered;
    bool started;
    float media_frame_rate;
    float surface_frame_rate;
};

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return MP_TIME_S_TO_NS(ts.tv_sec) + ts.tv_nsec;
}

static void on_vsync(int64_t frame_ns, void *data)
{
    struct vsync_sampler *s = data;
    if (atomic_load(&s->quit))
        return;

    atomic_store(&s->sampled_vsync_ns, frame_ns);
    if (s->measuring) {
        int64_t delta = frame_ns - s->last_frame_ns;
        int64_t current = atomic_load(&s->vsync_duration_ns);
        if (delta > 0 && (current == UNSET || delta < current * 3 / 2))
            atomic_store(&s->vsync_duration_ns, delta);
        s->measuring = false;
        s->post_delayed(s->choreographer, on_vsync, s, VSYNC_SAMPLE_UPDATE_PERIOD_MS);
    } else {
        s->measuring = true;
        s->post(s->choreographer, on_vsync, s);
    }
    s->last_frame_ns = frame_ns;
}

static MP_THREAD_VOID vsync_thread(void *arg)
{
    struct vsync_sampler *s = arg;
    mp_thread_set_name("vsync");

    ALooper *looper = ALooper_prepare(0);
    ALooper_acquire(looper);
    s->choreographer = AChoreographer_getInstance();

    mp_mutex_lock(&s->lock);
    s->looper = looper;
    s->started = true;
    mp_cond_signal(&s->ready);
    mp_mutex_unlock(&s->lock);

    if (s->choreographer)
        s->post(s->choreographer, on_vsync, s);
    while (!atomic_load(&s->quit))
        ALooper_pollOnce(-1, NULL, NULL, NULL);

    ALooper_release(looper);
    MP_THREAD_RETURN();
}

static void vsync_start(struct vo *vo, struct vsync_sampler *s)
{
    atomic_init(&s->quit, false);
    atomic_init(&s->sampled_vsync_ns, UNSET);
    atomic_init(&s->vsync_duration_ns, UNSET);
    s->post = (void *)dlsym(RTLD_DEFAULT, "AChoreographer_postFrameCallback64");
    s->post_delayed = (void *)dlsym(RTLD_DEFAULT,
                                    "AChoreographer_postFrameCallbackDelayed64");
    if (!s->post || !s->post_delayed) {
        MP_VERBOSE(vo, "AChoreographer 64-bit callbacks unavailable; not snapping to vsync\n");
        return;
    }

    mp_mutex_init(&s->lock);
    mp_cond_init(&s->ready);
    if (mp_thread_create(&s->thread, vsync_thread, s)) {
        mp_cond_destroy(&s->ready);
        mp_mutex_destroy(&s->lock);
        return;
    }
    mp_mutex_lock(&s->lock);
    while (!s->started)
        mp_cond_wait(&s->ready, &s->lock);
    mp_mutex_unlock(&s->lock);
    s->running = true;
}

static void vsync_stop(struct vsync_sampler *s)
{
    if (!s->running)
        return;
    atomic_store(&s->quit, true);
    ALooper_wake(s->looper);
    mp_thread_join(s->thread);
    mp_cond_destroy(&s->ready);
    mp_mutex_destroy(&s->lock);
    s->running = false;
}

static void release_reset(struct release_helper *r)
{
    *r = (struct release_helper){
        .last_frame_index = -1,
        .pending_frame_index = -1,
        .last_pts_us = UNSET,
        .pending_pts_us = UNSET,
    };
}

static int64_t closest_vsync(struct release_helper *r, int64_t release_ns,
                             int64_t sampled_ns, int64_t duration_ns)
{
    int64_t count = (release_ns - sampled_ns) / duration_ns;
    int64_t snapped = sampled_ns + duration_ns * count;
    int64_t before, after;
    if (release_ns <= snapped) {
        before = snapped - duration_ns;
        after = snapped;
    } else {
        before = snapped;
        after = snapped + duration_ns;
    }
    int64_t after_diff = after - release_ns;
    int64_t before_diff = release_ns - before;

    int64_t diffs_diff = llabs(after_diff - before_diff);
    if (diffs_diff < duration_ns / 2) {
        int64_t range = duration_ns / 4;
        if (diffs_diff < range) {
            r->pending_hysteresis_ns = r->last_hysteresis_ns
                ? r->last_hysteresis_ns
                : (after_diff < before_diff ? -range : range);
        } else {
            r->pending_hysteresis_ns = 0;
        }
    } else {
        r->pending_hysteresis_ns = r->last_hysteresis_ns;
    }
    return after_diff + r->pending_hysteresis_ns < before_diff ? after : before;
}

static int64_t adjust_release_time(struct priv *p, int64_t release_ns, int64_t pts_us,
                                   int64_t frame_duration_ns, int64_t frame_index)
{
    struct release_helper *r = &p->release;
    if (pts_us != r->pending_pts_us) {
        r->last_frame_index = r->pending_frame_index;
        r->last_release_ns = r->pending_release_ns;
        r->last_pts_us = r->pending_pts_us;
        r->last_hysteresis_ns = r->pending_hysteresis_ns;
    }

    int64_t adjusted = release_ns;
    if (r->last_frame_index >= 0) {
        int64_t elapsed = frame_duration_ns > 0
            ? frame_duration_ns * (frame_index - r->last_frame_index)
            : (pts_us - r->last_pts_us) * 1000;
        int64_t candidate = r->last_release_ns + elapsed;
        if (llabs(release_ns - candidate) <= MAX_ALLOWED_ADJUSTMENT_NS) {
            adjusted = candidate;
        } else {
            release_reset(r);
        }
    }
    r->pending_frame_index = frame_index;
    r->pending_release_ns = adjusted;
    r->pending_pts_us = pts_us;

    int64_t sampled = atomic_load(&p->vsync.sampled_vsync_ns);
    int64_t duration = atomic_load(&p->vsync.vsync_duration_ns);
    if (!p->vsync.running || sampled == UNSET || duration == UNSET)
        return adjusted;
    int64_t snapped = closest_vsync(r, adjusted, sampled, duration);
    return snapped - duration * VSYNC_OFFSET_PERCENTAGE / 100;
}

static void update_surface_frame_rate(struct vo *vo, bool force)
{
    struct priv *p = vo->priv;
    float rate = p->started && p->media_frame_rate > 0 ? p->media_frame_rate : 0;
    if (!force && rate == p->surface_frame_rate)
        return;
    p->surface_frame_rate = rate;
    vo_android_set_frame_rate(vo, rate);
}

static AVBufferRef *create_mediacodec_device_ref(struct vo *vo)
{
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!device_ref)
        return NULL;

    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVMediaCodecDeviceContext *hwctx = ctx->hwctx;
    mp_assert(vo->opts->WinID != 0 && vo->opts->WinID != -1);
    hwctx->surface = (void *)(intptr_t)(vo->opts->WinID);

    if (av_hwdevice_ctx_init(device_ref) < 0)
        av_buffer_unref(&device_ref);

    return device_ref;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!vo_android_init(vo))
        return -1;

    vo->hwdec_devs = hwdec_devices_create();
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = "mediacodec_embed",
        .av_device_ref = create_mediacodec_device_ref(vo),
        .hw_imgfmt = IMGFMT_MEDIACODEC,
    };

    if (!p->hwctx.av_device_ref) {
        MP_VERBOSE(vo, "Failed to create hwdevice_ctx\n");
        vo_android_uninit(vo);
        return -1;
    }

    hwdec_devices_add(vo->hwdec_devs, &p->hwctx);
    release_reset(&p->release);
    p->started = true;
    vsync_start(vo, &p->vsync);
    vo_set_queue_params(vo, EARLY_SCHEDULING_THRESHOLD_NS, 1);
    return 0;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!p->next_image)
        return;

    if (p->next_image->imgfmt != IMGFMT_MEDIACODEC ||
        !p->next_image->planes[3]) {
        MP_ERR(vo, "Invalid MediaCodec output frame\n");
        p->next_image_pts = 0;
        mp_image_unrefp(&p->next_image);
        return;
    }

    AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)p->next_image->planes[3];
    int err;
    if (p->next_image_pts <= 0 || !p->first_frame_rendered) {
        err = av_mediacodec_release_buffer(buffer, 1);
    } else {
        int64_t now = monotonic_ns();
        int64_t early = p->next_image_pts - mp_time_ns();
        int64_t pts_us = p->next_image->pts != MP_NOPTS_VALUE
                         ? llrint(p->next_image->pts * 1e6) : UNSET;
        int64_t release = adjust_release_time(p, now + early, pts_us,
                                              p->next_frame_duration_ns,
                                              p->frame_index);
        if (release - now < LATE_THRESHOLD_NS) {
            err = av_mediacodec_release_buffer(buffer, 0);
            vo_increment_drop_count(vo, 1);
        } else {
            err = av_mediacodec_render_buffer_at_time(buffer, release);
        }
    }
    if (err < 0)
        MP_WARN(vo, "Failed to release MediaCodec output buffer: %d\n", err);

    p->first_frame_rendered = true;
    p->frame_index++;
    p->next_image_pts = 0;
    mp_image_unrefp(&p->next_image);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

    mp_image_t *mpi = NULL;
    if (!frame->redraw && !frame->repeat)
        mpi = mp_image_new_ref(frame->current);

    talloc_free(p->next_image);
    p->next_image = mpi;
    p->next_image_pts = mpi ? frame->pts : 0;
    p->next_frame_duration_ns = frame->duration > 0 ? llrint(frame->duration) : -1;
    if (mpi) {
        vo_android_set_buffers_dataspace(vo, &mpi->params);
        float rate = mpi->nominal_fps;
        if (rate > 0 && frame->duration > 0) {
            double speed = (1e9 / rate) / frame->duration;
            if (fabs(speed - 1) > 0.02)
                rate *= speed;
        }
        if (rate > 0 && fabsf(rate - p->media_frame_rate) > 0.01f) {
            p->media_frame_rate = rate;
            update_surface_frame_rate(vo, false);
        }
    }
    return VO_TRUE;
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT_MEDIACODEC;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;
    switch (request) {
    case VOCTRL_RESET:
        release_reset(&p->release);
        p->first_frame_rendered = false;
        return VO_TRUE;
    case VOCTRL_PAUSE:
        p->started = false;
        update_surface_frame_rate(vo, false);
        return VO_TRUE;
    case VOCTRL_RESUME:
        p->started = true;
        release_reset(&p->release);
        update_surface_frame_rate(vo, true);
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    vo_android_set_buffers_dataspace(vo, params);
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    mp_image_unrefp(&p->next_image);

    vsync_stop(&p->vsync);
    p->started = false;
    update_surface_frame_rate(vo, true);
    hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
    vo_android_uninit(vo);
}

const struct vo_driver video_out_mediacodec_embed = {
    .description = "Android (Embedded MediaCodec Surface)",
    .name = "mediacodec_embed",
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
