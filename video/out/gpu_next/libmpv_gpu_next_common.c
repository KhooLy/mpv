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

#include "common/common.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "video/out/vo.h"
#include "video/out/gpu/hwdec.h"

#include "libmpv_gpu_next_common.h"
#include "video.h"

int lgn_common_init(struct render_backend *ctx, struct ra_ctx *ra_ctx)
{
    struct gpu_next_priv *p = ctx->priv;

    p->gpu_ctx = (struct gpu_ctx){
        .log = ctx->log,
        .ra_ctx = ra_ctx,
        .pllog = p->pllog,
        .gpu = p->gpu,
    };
    p->video = mppl_video_init(ctx->log, ctx->global, &p->gpu_ctx);

    ctx->hwdec_devs = hwdec_devices_create();
    mppl_video_init_hwdecs(p->video, ctx->hwdec_devs, true);

    ctx->driver_caps = VO_CAP_ROTATE90 | VO_CAP_VFLIP;
    return 0;
}

bool lgn_check_format(struct render_backend *ctx, int imgfmt)
{
    struct gpu_next_priv *p = ctx->priv;
    return mppl_video_check_format(p->video, imgfmt);
}

int lgn_set_parameter(struct render_backend *ctx, mpv_render_param param)
{
    struct gpu_next_priv *p = ctx->priv;
    switch (param.type) {
    case MPV_RENDER_PARAM_ICC_PROFILE: {
        mpv_byte_array *data = param.data;
        bstr icc = {talloc_memdup(NULL, data->data, data->size), data->size};
        mppl_video_set_icc_profile(p->video, icc);
        return 0;
    }
    default:
        return MPV_ERROR_NOT_IMPLEMENTED;
    }
}

void lgn_reconfig(struct render_backend *ctx, struct mp_image_params *params)
{
    struct gpu_next_priv *p = ctx->priv;
    if (params)
        mppl_video_config(p->video, params);
}

void lgn_reset(struct render_backend *ctx)
{
    struct gpu_next_priv *p = ctx->priv;
    mppl_video_reset(p->video);
}

void lgn_update_external(struct render_backend *ctx, struct vo *vo)
{
    struct gpu_next_priv *p = ctx->priv;
    p->vo = vo;
    mppl_video_set_osd_source(p->video, vo ? vo->osd : NULL);
    if (vo)
        mppl_video_configure_queue(p->video, vo);
}

void lgn_resize(struct render_backend *ctx, struct mp_rect *src,
                struct mp_rect *dst, struct mp_osd_res *osd)
{
    struct gpu_next_priv *p = ctx->priv;
    mppl_video_resize(p->video, *src, *dst, *osd);
}

static void apply_render_target_color(struct pl_color_space *csp, mpv_render_color *c)
{
    static const enum pl_color_transfer trc[] = {
        [MPV_RENDER_TRANSFER_SDR] = PL_COLOR_TRC_SRGB,
        [MPV_RENDER_TRANSFER_PQ] = PL_COLOR_TRC_PQ,
        [MPV_RENDER_TRANSFER_HLG] = PL_COLOR_TRC_HLG,
    };
    static const enum pl_color_primaries prim[] = {
        [MPV_RENDER_PRIMARIES_BT709] = PL_COLOR_PRIM_BT_709,
        [MPV_RENDER_PRIMARIES_BT2020] = PL_COLOR_PRIM_BT_2020,
        [MPV_RENDER_PRIMARIES_DISPLAY_P3] = PL_COLOR_PRIM_DISPLAY_P3,
    };
    if ((unsigned)c->transfer < MP_ARRAY_SIZE(trc))
        csp->transfer = trc[c->transfer];
    if ((unsigned)c->primaries < MP_ARRAY_SIZE(prim))
        csp->primaries = prim[c->primaries];
    if (c->max_luma > 0)
        csp->hdr.max_luma = c->max_luma;
    if (c->min_luma > 0)
        csp->hdr.min_luma = c->min_luma;
}

bool lgn_render_frame(struct render_backend *ctx, mpv_render_param *params,
                      struct vo_frame *frame, pl_tex target_tex, bool flip,
                      int w, int h)
{
    struct gpu_next_priv *p = ctx->priv;

    struct pl_color_space display = pl_color_space_srgb;
    mpv_render_color *target_color =
        get_mpv_render_param(params, MPV_RENDER_PARAM_TARGET_COLOR, NULL);
    if (target_color)
        apply_render_target_color(&display, target_color);

    int depth = GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_DEPTH, int, 0);
    struct pl_color_repr repr = pl_color_repr_rgb;
    if (depth > 0) {
        repr.bits.color_depth = depth;
        repr.bits.sample_depth = depth;
    }

    mppl_video_begin_frame(p->video, frame, display, 0);
    if (p->vo)
        mppl_video_configure_queue(p->video, p->vo);

    struct pl_swapchain_frame swframe = {
        .fbo = target_tex,
        .flipped = flip,
        .color_repr = repr,
        .color_space = display,
    };
    if (p->pre_render)
        p->pre_render(p);

    struct mp_image_params target_params;
    bool ok = mppl_video_render_frame(p->video, frame, &swframe, depth,
                                      target_color ? &display : NULL,
                                      &target_params);
    if (ok && p->vo) {
        mp_mutex_lock(&p->vo->params_mutex);
        p->target_params = target_params;
        p->vo->target_params = &p->target_params;
        if (p->vo->params) {
            p->vo->has_peak_detect_values =
                mppl_video_get_hdr_metadata(p->video, &p->vo->params->color.hdr);
        }
        mp_mutex_unlock(&p->vo->params_mutex);
    }
    return ok;
}

struct mp_image *lgn_get_image(struct render_backend *ctx, int imgfmt,
                               int w, int h, int stride_align, int flags)
{
    return NULL;
}

void lgn_screenshot(struct render_backend *ctx, struct vo_frame *frame,
                    struct voctrl_screenshot *args)
{
    struct gpu_next_priv *p = ctx->priv;
    if (p->pre_render)
        p->pre_render(p);
    mppl_video_screenshot(p->video, args);
}

void lgn_perfdata(struct render_backend *ctx, struct voctrl_performance_data *out)
{
    struct gpu_next_priv *p = ctx->priv;
    mppl_video_perfdata(p->video, out);
}

void lgn_common_uninit(struct render_backend *ctx)
{
    struct gpu_next_priv *p = ctx->priv;
    if (!p->video)
        return;
    mppl_video_uninit(p->video);
    p->video = NULL;
    hwdec_devices_destroy(ctx->hwdec_devs);
}
