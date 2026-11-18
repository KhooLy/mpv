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

#include <libplacebo/renderer.h>

#include "video/out/gpu_next/context.h"
#include "video/out/libmpv.h"

struct mppl_video;

struct gpu_next_priv {
    struct mpv_global *global;
    struct mp_log *log;

    pl_log pllog;
    pl_gpu gpu;
    struct gpu_ctx gpu_ctx;
    struct mppl_video *video;
    struct vo *vo;
    struct mp_image_params target_params;

    void *api_priv;
    void (*pre_render)(struct gpu_next_priv *p);
};

int lgn_common_init(struct render_backend *ctx, struct ra_ctx *ra_ctx);
bool lgn_render_frame(struct render_backend *ctx, mpv_render_param *params,
                      struct vo_frame *frame, pl_tex target_tex, bool flip,
                      int w, int h);

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
void lgn_common_uninit(struct render_backend *ctx);
