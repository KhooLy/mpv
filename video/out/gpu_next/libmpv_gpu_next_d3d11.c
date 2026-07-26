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

#include <libplacebo/d3d11.h>

#include "common/common.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"
#include "mpv/render_d3d11.h"

#include "libmpv_gpu_next_common.h"

struct d3d11_api_priv {
    pl_d3d11 d3d11;
    struct ra_ctx *ra_ctx; // minimal, headless: only .ra/.global/.log are set
};

static pl_tex wrap_hwdec_tex(struct gpu_next_priv *p, struct ra_tex *ratex)
{
    return (pl_tex)ratex->priv;
}

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct gpu_next_priv);
    struct gpu_next_priv *p = ctx->priv;
    p->global = ctx->global;
    p->log = ctx->log;
    struct d3d11_api_priv *a = talloc_zero(p, struct d3d11_api_priv);
    p->api_priv = a;
    p->wrap_hwdec_tex = wrap_hwdec_tex;

    const char *backend = getenv("MPV_LIBMPV_RENDER_BACKEND");
    char *api = get_mpv_render_param(params, MPV_RENDER_PARAM_API_TYPE, NULL);
    MP_WARN(ctx->log, "gpu-next D3D11 init: MPV_LIBMPV_RENDER_BACKEND='%s', api='%s'\n",
            backend ? backend : "<unset>", api ? api : "<null>");
    if (!backend || strcmp(backend, "gpu-next") != 0)
        return MPV_ERROR_NOT_IMPLEMENTED;

    if (!api || strcmp(api, MPV_RENDER_API_TYPE_D3D11) != 0)
        return MPV_ERROR_NOT_IMPLEMENTED;

    mpv_d3d11_init_params *init_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_D3D11_INIT_PARAMS, NULL);
    if (!init_params || !init_params->device)
        return MPV_ERROR_INVALID_PARAMETER;

    p->pllog = mppl_log_create(p, ctx->log);
    if (!p->pllog)
        return MPV_ERROR_UNSUPPORTED;

    a->d3d11 = pl_d3d11_create(p->pllog, pl_d3d11_params(
        .device = init_params->device,
    ));
    if (!a->d3d11)
        return MPV_ERROR_UNSUPPORTED;
    p->gpu = a->d3d11->gpu;

    a->ra_ctx = talloc_zero(p, struct ra_ctx);
    a->ra_ctx->ra = ra_create_pl(p->gpu, ctx->log);
    a->ra_ctx->global = ctx->global;
    a->ra_ctx->log = ctx->log;
    if (!a->ra_ctx->ra)
        return MPV_ERROR_UNSUPPORTED;

    return lgn_common_init(ctx, a->ra_ctx);
}

static int get_target_size(struct render_backend *ctx, mpv_render_param *params,
                           int *out_w, int *out_h)
{
    mpv_d3d11_target *tgt = get_mpv_render_param(params, MPV_RENDER_PARAM_D3D11_TARGET, NULL);
    if (!tgt)
        return MPV_ERROR_INVALID_PARAMETER;
    *out_w = tgt->w;
    *out_h = tgt->h;
    return 0;
}

static int render(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame)
{
    struct gpu_next_priv *p = ctx->priv;

    mpv_d3d11_target *tgt = get_mpv_render_param(params, MPV_RENDER_PARAM_D3D11_TARGET, NULL);
    if (!tgt || !tgt->tex)
        return MPV_ERROR_INVALID_PARAMETER;

    pl_tex target_tex = pl_d3d11_wrap(p->gpu, pl_d3d11_wrap_params(
        .tex = (ID3D11Resource *)tgt->tex,
        .fmt = tgt->format,
        .w = tgt->w,
        .h = tgt->h,
    ));
    if (!target_tex)
        return MPV_ERROR_GENERIC;

    bool ok = lgn_render_frame(ctx, params, frame, target_tex, false, tgt->w, tgt->h);

    pl_tex_destroy(p->gpu, &target_tex);
    pl_gpu_flush(p->gpu);
    return ok ? 0 : MPV_ERROR_GENERIC;
}

static void destroy(struct render_backend *ctx)
{
    struct gpu_next_priv *p = ctx->priv;
    struct d3d11_api_priv *a = p->api_priv;

    lgn_common_uninit(ctx);

    if (a->ra_ctx && a->ra_ctx->ra) {
        a->ra_ctx->ra->fns->destroy(a->ra_ctx->ra);
        a->ra_ctx->ra = NULL;
    }
    pl_d3d11_destroy(&a->d3d11);
    if (p->pllog)
        pl_log_destroy(&p->pllog);
}

const struct render_backend_fns render_backend_gpu_next_d3d11 = {
    .init = init,
    .check_format = lgn_check_format,
    .set_parameter = lgn_set_parameter,
    .reconfig = lgn_reconfig,
    .reset = lgn_reset,
    .update_external = lgn_update_external,
    .resize = lgn_resize,
    .get_target_size = get_target_size,
    .render = render,
    .get_image = lgn_get_image,
    .screenshot = lgn_screenshot,
    .perfdata = lgn_perfdata,
    .destroy = destroy,
};
