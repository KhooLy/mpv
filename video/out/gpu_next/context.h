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
 * License along with mpv.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/lut.h>

#include "options/m_option.h"

struct mp_log;
struct ra;
struct ra_ctx;
struct ra_tex;

struct user_lut {
    char *opt;
    char *path;
    int type;
    struct pl_custom_lut *lut;
};

struct gl_next_opts {
    struct pl_tone_map_constants tone_constants;
    bool delayed_peak;
    int sub_hdr_peak;
    int image_subs_hdr_peak;
    int border_background;
    float background_blur_radius;
    float corner_rounding;
    bool inter_preserve;
    struct user_lut lut;
    struct user_lut image_lut;
    struct user_lut target_lut;
    int target_hint;
    int target_hint_mode;
    bool target_hint_strict;
    char **raw_opts;
};

extern const struct m_sub_options gl_next_conf;

struct gpu_ctx {
    struct mp_log *log;
    struct ra_ctx *ra_ctx;

    pl_log pllog;
    pl_gpu gpu;
    pl_swapchain swapchain;

    void *priv;
};

// The ra_ctx has to outlive the returned context.
struct gpu_ctx *gpu_ctx_create(struct mp_log *log, struct ra_ctx *ra_ctx, bool probing);
bool gpu_ctx_create_swapchain(struct gpu_ctx *ctx, int swapchain_depth);
bool gpu_ctx_resize(struct gpu_ctx *ctx, int w, int h);
pl_tex gpu_ctx_wrap_ra_tex(pl_gpu gpu, struct ra *ra, struct ra_tex *ratex);
void gpu_ctx_unwrap_ra_tex(pl_gpu gpu, struct ra *ra, pl_tex *tex);
void gpu_ctx_destroy(struct gpu_ctx **ctxp);
