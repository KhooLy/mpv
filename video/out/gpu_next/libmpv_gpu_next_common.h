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

#pragma once

#include <libplacebo/options.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/icc.h>
#include <libplacebo/utils/frame_queue.h>

#include "video/out/gpu/hwdec.h"
#include "video/out/gpu/video.h"
#include "video/out/gpu_next/context.h"
#include "video/out/libmpv.h"

// Matches gl_video.h / vo_gpu_next.c's private OSD overlay slot count.
#define MAX_GPU_NEXT_OSD_PARTS 64

struct lgn_osd_entry {
    pl_tex tex;
    struct pl_overlay_part *parts;
    int num_parts;
};

struct lgn_overlay_state {
    struct lgn_osd_entry entries[MAX_GPU_NEXT_OSD_PARTS];
    struct pl_overlay overlays[MAX_GPU_NEXT_OSD_PARTS];
};

struct lgn_scaler_params {
    struct pl_filter_config config;
};

struct lgn_user_hook {
    char *path;
    const struct pl_hook *hook;
};

struct gpu_next_priv {
    struct mpv_global *global;
    struct mp_log *log;

    pl_log pllog;
    pl_gpu gpu;
    pl_renderer rr;
    pl_queue queue;
    pl_options pars;
    struct lgn_scaler_params scalers[SCALER_COUNT];

    struct ra_hwdec_ctx hwdec_ctx;
    struct ra_hwdec_mapper *hwdec_mapper;
    struct ra_hwdec *hwdec;

    pl_fmt osd_fmt[SUBBITMAP_COUNT];
    pl_tex *sub_tex;
    int num_sub_tex;
    struct lgn_overlay_state overlays;
    struct osd_state *osd;
    struct vo *vo;
    struct mp_image_params target_params;
    struct mp_rect src, dst;
    struct mp_osd_res osd_res;

    pl_icc_object icc_profile;
    struct pl_icc_params icc_params;

    struct m_config_cache *opts_cache; // gl_video_conf: --tone-mapping, --target-*, --deband, --dither, etc.
    struct m_config_cache *next_opts_cache; // gl_next_conf: --lut, --image-lut, --target-lut, etc.
    struct gl_next_opts *next_opts;

    struct lgn_user_hook *user_hooks;
    int num_user_hooks;
    const struct pl_hook **hooks; // storage for pars->params.hooks

    uint64_t last_id;
    double last_pts;
    bool want_reset;
    bool flush_cache;

    void *api_priv;
    // Backend-provided hooks. wrap_hwdec_tex/release_hwdec_planes adapt how a
    // mapped ra_tex becomes a pl_tex; pre_render (optional) runs right before
    // pl_render_image_mix / pl_render_image (GL uses it to kill FRAMEBUFFER_SRGB).
    pl_tex (*wrap_hwdec_tex)(struct gpu_next_priv *p, struct ra_tex *ratex);
    void (*release_hwdec_planes)(struct gpu_next_priv *p, struct pl_frame *frame);
    void (*pre_render)(struct gpu_next_priv *p);
};

// Shared setup after the backend has created p->pllog and p->gpu. Takes
// ownership of nothing; ra_ctx is only borrowed for hwdec init.
int lgn_common_init(struct render_backend *ctx, struct ra_ctx *ra_ctx);

// Everything between "the target tex is wrapped" and "the backend finishes the
// frame": opts refresh, queue push/update, overlays, render, target_params.
// Clears target_tex on failure. Does not destroy target_tex or flush the gpu.
bool lgn_render_frame(struct render_backend *ctx, mpv_render_param *params,
                      struct vo_frame *frame, pl_tex target_tex, bool flip,
                      int w, int h);

// render_backend_fns entries that need no per-API code.
bool lgn_check_format(struct render_backend *ctx, int imgfmt);
int lgn_set_parameter(struct render_backend *ctx, mpv_render_param param);
void lgn_reconfig(struct render_backend *ctx, struct mp_image_params *params);
void lgn_reset(struct render_backend *ctx);
void lgn_update_external(struct render_backend *ctx, struct vo *vo);
void lgn_resize(struct render_backend *ctx, struct mp_rect *src,
                struct mp_rect *dst, struct mp_osd_res *osd);
struct mp_image *lgn_get_image(struct render_backend *ctx, int imgfmt,
                               int w, int h, int stride_align, int flags);
void lgn_screenshot(struct render_backend *ctx, struct vo_frame *frame,
                    struct voctrl_screenshot *args);
void lgn_perfdata(struct render_backend *ctx, struct voctrl_performance_data *out);

// Tears down everything lgn_common_init and rendering created. The backend
// destroys its own API objects (and p->pllog) afterwards.
void lgn_common_uninit(struct render_backend *ctx);
