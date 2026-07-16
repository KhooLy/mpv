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

#include <libplacebo/vulkan.h>

#include "common/common.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"
#include "mpv/render_vk.h"

#include "libmpv_gpu_next_common.h"

struct vk_api_priv {
    pl_vulkan vk;
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
    struct vk_api_priv *a = talloc_zero(p, struct vk_api_priv);
    p->api_priv = a;
    p->wrap_hwdec_tex = wrap_hwdec_tex;

    const char *backend = getenv("MPV_LIBMPV_RENDER_BACKEND");
    if (!backend || strcmp(backend, "gpu-next") != 0)
        return MPV_ERROR_NOT_IMPLEMENTED;

    char *api = get_mpv_render_param(params, MPV_RENDER_PARAM_API_TYPE, NULL);
    if (!api || strcmp(api, MPV_RENDER_API_TYPE_VULKAN) != 0)
        return MPV_ERROR_NOT_IMPLEMENTED;

    mpv_vulkan_init_params *init_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, NULL);
    if (!init_params || !init_params->instance || !init_params->phys_device ||
        !init_params->device)
        return MPV_ERROR_INVALID_PARAMETER;

    p->pllog = mppl_log_create(p, ctx->log);
    if (!p->pllog)
        return MPV_ERROR_UNSUPPORTED;

    a->vk = pl_vulkan_import(p->pllog, pl_vulkan_import_params(
        .instance = init_params->instance,
        .get_proc_addr = init_params->get_proc_address,
        .phys_device = init_params->phys_device,
        .device = init_params->device,
        .extensions = init_params->enabled_extensions,
        .num_extensions = init_params->num_enabled_extensions,
        .queue_graphics = {
            .index = init_params->queue_graphics_index,
            .count = MPMAX(init_params->queue_graphics_count, 1),
        },
        .features = &pl_vulkan_required_features,
    ));
    if (!a->vk)
        return MPV_ERROR_UNSUPPORTED;
    p->gpu = a->vk->gpu;

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
    mpv_vulkan_image *img = get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_IMAGE, NULL);
    if (!img)
        return MPV_ERROR_INVALID_PARAMETER;
    *out_w = img->w;
    *out_h = img->h;
    return 0;
}

static int render(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame)
{
    struct gpu_next_priv *p = ctx->priv;

    mpv_vulkan_image *img = get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_IMAGE, NULL);
    if (!img)
        return MPV_ERROR_INVALID_PARAMETER;
    if (!img->signal_semaphore)
        return MPV_ERROR_INVALID_PARAMETER;

    pl_tex target_tex = pl_vulkan_wrap(p->gpu, pl_vulkan_wrap_params(
        .image = img->image,
        .width = img->w,
        .height = img->h,
        .format = img->format,
        .usage = img->usage,
    ));
    if (!target_tex)
        return MPV_ERROR_GENERIC;

    pl_vulkan_release_ex(p->gpu, pl_vulkan_release_params(
        .tex = target_tex,
        .layout = img->layout,
        .qf = VK_QUEUE_FAMILY_IGNORED,
        .semaphore = { .sem = img->wait_semaphore },
    ));

    bool ok = lgn_render_frame(ctx, params, frame, target_tex, false, img->w, img->h);

    VkImageLayout out_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    pl_vulkan_hold_ex(p->gpu, pl_vulkan_hold_params(
        .tex = target_tex,
        .out_layout = &out_layout,
        .qf = VK_QUEUE_FAMILY_IGNORED,
        .semaphore = { .sem = img->signal_semaphore },
    ));
    img->layout = out_layout;

    pl_tex_destroy(p->gpu, &target_tex);
    pl_gpu_flush(p->gpu);
    return ok ? 0 : MPV_ERROR_GENERIC;
}

static void destroy(struct render_backend *ctx)
{
    struct gpu_next_priv *p = ctx->priv;
    struct vk_api_priv *a = p->api_priv;

    lgn_common_uninit(ctx);

    if (a->ra_ctx && a->ra_ctx->ra) {
        a->ra_ctx->ra->fns->destroy(a->ra_ctx->ra);
        a->ra_ctx->ra = NULL;
    }
    pl_vulkan_destroy(&a->vk);
    if (p->pllog)
        pl_log_destroy(&p->pllog);
}

const struct render_backend_fns render_backend_gpu_next_vk = {
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
