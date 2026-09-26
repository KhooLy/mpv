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
#include <string.h>
#include <time.h>

#include <android/choreographer.h>
#include <android/hardware_buffer.h>
#include <android/looper.h>
#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>

#include "common/common.h"
#include "sub/osd.h"
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

typedef struct ASurfaceControl ASurfaceControl;
typedef struct ASurfaceTransaction ASurfaceTransaction;

struct osd_layer {
    ASurfaceControl *(*create)(ANativeWindow *, const char *);
    void (*release)(ASurfaceControl *);
    ASurfaceTransaction *(*txn_create)(void);
    void (*txn_delete)(ASurfaceTransaction *);
    void (*txn_apply)(ASurfaceTransaction *);
    void (*set_buffer)(ASurfaceTransaction *, ASurfaceControl *, AHardwareBuffer *, int);
    void (*set_geometry)(ASurfaceTransaction *, ASurfaceControl *, const ARect *,
                         const ARect *, int32_t);
    void (*set_visibility)(ASurfaceTransaction *, ASurfaceControl *, int8_t);
    void (*set_z_order)(ASurfaceTransaction *, ASurfaceControl *, int32_t);
    void (*set_transparency)(ASurfaceTransaction *, ASurfaceControl *, int8_t);
    void (*set_present_time)(ASurfaceTransaction *, int64_t);
    void (*reparent)(ASurfaceTransaction *, ASurfaceControl *, ASurfaceControl *);

    ASurfaceControl *sc;
    struct mp_osd_res res;
    int buf_w, buf_h;
    uint8_t *scratch;
    int64_t change_id;
    bool visible;
    double pts;
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
    struct osd_layer osd;
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

static void osd_init(struct vo *vo)
{
    struct osd_layer *o = &((struct priv *)vo->priv)->osd;
    void *lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib)
        return;
    o->create = dlsym(lib, "ASurfaceControl_createFromWindow");
    o->release = dlsym(lib, "ASurfaceControl_release");
    o->txn_create = dlsym(lib, "ASurfaceTransaction_create");
    o->txn_delete = dlsym(lib, "ASurfaceTransaction_delete");
    o->txn_apply = dlsym(lib, "ASurfaceTransaction_apply");
    o->set_buffer = dlsym(lib, "ASurfaceTransaction_setBuffer");
    o->set_geometry = dlsym(lib, "ASurfaceTransaction_setGeometry");
    o->set_visibility = dlsym(lib, "ASurfaceTransaction_setVisibility");
    o->set_z_order = dlsym(lib, "ASurfaceTransaction_setZOrder");
    o->set_transparency = dlsym(lib, "ASurfaceTransaction_setBufferTransparency");
    o->set_present_time = dlsym(lib, "ASurfaceTransaction_setDesiredPresentTime");
    o->reparent = dlsym(lib, "ASurfaceTransaction_reparent");
    dlclose(lib);
    if (!o->create || !o->release || !o->txn_create || !o->txn_delete ||
        !o->txn_apply || !o->set_buffer || !o->set_geometry || !o->set_visibility ||
        !o->set_z_order || !o->set_transparency || !o->set_present_time || !o->reparent)
    {
        MP_WARN(vo, "ASurfaceControl unavailable (Android 10+ required); "
                "subtitles and OSD will not be shown\n");
        return;
    }

    o->sc = o->create(vo_android_native_window(vo), "mpv-osd");
    if (!o->sc) {
        MP_WARN(vo, "Failed to create subtitle surface\n");
        return;
    }
    o->change_id = -1;
    ASurfaceTransaction *t = o->txn_create();
    o->set_z_order(t, o->sc, 1);
    o->set_transparency(t, o->sc, 1);
    o->set_visibility(t, o->sc, 0);
    o->txn_apply(t);
    o->txn_delete(t);
}

static void osd_uninit(struct osd_layer *o)
{
    if (!o->sc)
        return;
    ASurfaceTransaction *t = o->txn_create();
    o->reparent(t, o->sc, NULL);
    o->txn_apply(t);
    o->txn_delete(t);
    o->release(o->sc);
    o->sc = NULL;
}

static void blend_part(uint8_t *dst, int dst_stride, struct mp_rect box,
                       const struct sub_bitmap *b)
{
    int x0 = MPMAX(b->x, box.x0), x1 = MPMIN(b->x + b->dw, box.x1);
    int y0 = MPMAX(b->y, box.y0), y1 = MPMIN(b->y + b->dh, box.y1);
    for (int y = y0; y < y1; y++) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)b->bitmap +
                              (size_t)((y - b->y) * b->h / b->dh) * b->stride);
        uint8_t *d = dst + (size_t)(y - box.y0) * dst_stride + (size_t)(x0 - box.x0) * 4;
        for (int x = x0; x < x1; x++, d += 4) {
            uint32_t c = src[(x - b->x) * b->w / b->dw];
            unsigned a = c >> 24;
            if (!a)
                continue;
            unsigned inv = 255 - a;
            d[0] = ((c >> 16) & 0xff) + (d[0] * inv + 127) / 255;
            d[1] = ((c >> 8) & 0xff) + (d[1] * inv + 127) / 255;
            d[2] = (c & 0xff) + (d[2] * inv + 127) / 255;
            d[3] = a + (d[3] * inv + 127) / 255;
        }
    }
}

static void blend_ass(uint8_t *dst, int dst_stride, struct mp_rect box,
                      const struct sub_bitmap *b)
{
    unsigned a = 255 - (b->libass.color & 0xff);
    if (!a)
        return;
    unsigned r = (b->libass.color >> 24) & 0xff;
    unsigned g = (b->libass.color >> 16) & 0xff;
    unsigned bl = (b->libass.color >> 8) & 0xff;
    int x0 = MPMAX(b->x, box.x0), x1 = MPMIN(b->x + b->w, box.x1);
    int y0 = MPMAX(b->y, box.y0), y1 = MPMIN(b->y + b->h, box.y1);
    for (int y = y0; y < y1; y++) {
        const uint8_t *m = (const uint8_t *)b->bitmap + (size_t)(y - b->y) * b->stride +
                           (x0 - b->x);
        uint8_t *d = dst + (size_t)(y - box.y0) * dst_stride + (size_t)(x0 - box.x0) * 4;
        for (int x = x0; x < x1; x++, d += 4, m++) {
            unsigned k = *m * a;
            if (!k)
                continue;
            unsigned inv = 255 * 255 - k;
            d[0] = (r * k + d[0] * inv) / (255 * 255);
            d[1] = (g * k + d[1] * inv) / (255 * 255);
            d[2] = (bl * k + d[2] * inv) / (255 * 255);
            d[3] = (k * 255 + d[3] * inv) / (255 * 255);
        }
    }
}

static void draw_osd(struct vo *vo, int64_t present_ns)
{
    struct priv *p = vo->priv;
    struct osd_layer *o = &p->osd;
    if (!o->sc || !vo->osd || o->buf_w <= 0)
        return;

    struct mp_osd_res res = {.w = o->buf_w, .h = o->buf_h, .display_par = 1};
    if (!osd_res_equals(res, o->res)) {
        o->res = res;
        o->change_id = -1;
    }

    static const bool formats[SUBBITMAP_COUNT] = {
        [SUBBITMAP_LIBASS] = true,
        [SUBBITMAP_BGRA] = true,
    };
    struct sub_bitmap_list *list = osd_render(vo->osd, res, o->pts, 0, formats);
    if (list->change_id == o->change_id) {
        talloc_free(list);
        return;
    }
    o->change_id = list->change_id;

    struct mp_rect box = {res.w, res.h, 0, 0};
    for (int n = 0; n < list->num_items; n++) {
        struct sub_bitmaps *imgs = list->items[n];
        for (int i = 0; i < imgs->num_parts; i++) {
            struct sub_bitmap *b = &imgs->parts[i];
            if (b->dw <= 0 || b->dh <= 0)
                continue;
            box.x0 = MPMIN(box.x0, b->x);
            box.y0 = MPMIN(box.y0, b->y);
            box.x1 = MPMAX(box.x1, b->x + b->dw);
            box.y1 = MPMAX(box.y1, b->y + b->dh);
        }
    }
    box.x0 = MPMAX(box.x0, 0);
    box.y0 = MPMAX(box.y0, 0);
    box.x1 = MPMIN(box.x1, res.w);
    box.y1 = MPMIN(box.y1, res.h);

    AHardwareBuffer *buf = NULL;
    if (box.x1 > box.x0 && box.y1 > box.y0) {
        AHardwareBuffer_Desc desc = {
            .width = box.x1 - box.x0,
            .height = box.y1 - box.y0,
            .layers = 1,
            .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            .usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                     AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY,
        };
        int stride = desc.width * 4;
        size_t size = (size_t)stride * desc.height;
        if (talloc_get_size(o->scratch) < size) {
            talloc_free(o->scratch);
            o->scratch = talloc_size(p, size);
        }
        memset(o->scratch, 0, size);
        for (int n = 0; n < list->num_items; n++) {
            struct sub_bitmaps *imgs = list->items[n];
            for (int i = 0; i < imgs->num_parts; i++) {
                struct sub_bitmap *b = &imgs->parts[i];
                if (imgs->format == SUBBITMAP_LIBASS)
                    blend_ass(o->scratch, stride, box, b);
                else if (b->dw > 0 && b->dh > 0)
                    blend_part(o->scratch, stride, box, b);
            }
        }

        void *data = NULL;
        if (AHardwareBuffer_allocate(&desc, &buf) == 0) {
            AHardwareBuffer_describe(buf, &desc);
            if (AHardwareBuffer_lock(buf, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                                     -1, NULL, &data) != 0)
                data = NULL;
        }
        if (data) {
            for (uint32_t y = 0; y < desc.height; y++) {
                memcpy((uint8_t *)data + (size_t)y * desc.stride * 4,
                       o->scratch + (size_t)y * stride, stride);
            }
            AHardwareBuffer_unlock(buf, NULL);
        } else if (buf) {
            MP_WARN(vo, "Failed to map subtitle buffer\n");
            AHardwareBuffer_release(buf);
            buf = NULL;
        }
    }
    talloc_free(list);

    if (!buf && !o->visible)
        return;

    ASurfaceTransaction *t = o->txn_create();
    if (buf) {
        ARect src = {0, 0, box.x1 - box.x0, box.y1 - box.y0};
        ARect dst = {box.x0, box.y0, box.x1, box.y1};
        o->set_buffer(t, o->sc, buf, -1);
        o->set_geometry(t, o->sc, &src, &dst, 0);
    }
    if (o->visible != !!buf)
        o->set_visibility(t, o->sc, !!buf);
    o->visible = !!buf;
    if (present_ns > 0)
        o->set_present_time(t, present_ns);
    o->txn_apply(t);
    o->txn_delete(t);
    if (buf)
        AHardwareBuffer_release(buf);
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
    osd_init(vo);
    vo_set_queue_params(vo, EARLY_SCHEDULING_THRESHOLD_NS, 1);
    return 0;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!p->next_image) {
        draw_osd(vo, 0);
        return;
    }

    if (p->next_image->imgfmt != IMGFMT_MEDIACODEC ||
        !p->next_image->planes[3]) {
        MP_ERR(vo, "Invalid MediaCodec output frame\n");
        p->next_image_pts = 0;
        mp_image_unrefp(&p->next_image);
        draw_osd(vo, 0);
        return;
    }

    AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)p->next_image->planes[3];
    int err;
    int64_t present_ns = 0;
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
            present_ns = release;
        }
    }
    if (err < 0)
        MP_WARN(vo, "Failed to release MediaCodec output buffer: %d\n", err);

    p->first_frame_rendered = true;
    p->frame_index++;
    p->next_image_pts = 0;
    mp_image_unrefp(&p->next_image);
    draw_osd(vo, present_ns);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

    mp_image_t *mpi = NULL;
    if (!frame->redraw && !frame->repeat && frame->current)
        mpi = mp_image_new_ref(frame->current);

    talloc_free(p->next_image);
    p->next_image = mpi;
    p->next_image_pts = mpi ? frame->pts : 0;
    if (frame->current)
        p->osd.pts = frame->current->pts;
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
    struct priv *p = vo->priv;
    p->osd.buf_w = params->w;
    p->osd.buf_h = params->h;
    p->osd.change_id = -1;
    vo_android_set_buffers_dataspace(vo, params);
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    mp_image_unrefp(&p->next_image);

    osd_uninit(&p->osd);
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
