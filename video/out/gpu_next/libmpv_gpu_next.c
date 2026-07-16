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

#include <libplacebo/opengl.h>

#include "common/common.h"
#include "video/out/gpu/libmpv_gpu.h"
#include "video/out/opengl/ra_gl.h"
#include "video/out/placebo/utils.h"
#include "mpv/render_gl.h"

#include "libmpv_gpu_next_common.h"

#if HAVE_EGL
#include <EGL/egl.h>
#endif

// libplacebo never touches this state itself, so we must.
#define GL_FRAMEBUFFER_SRGB 0x8DB9

struct gl_api_priv {
    struct libmpv_gpu_context *context; // owns the embeddable GL ra_ctx
    struct GL *gl;
    pl_opengl opengl;
};

static pl_tex wrap_hwdec_tex(struct gpu_next_priv *p, struct ra_tex *ratex)
{
    struct ra *ra = p->hwdec_mapper->ra;
    struct pl_opengl_wrap_params par = {
        .width = ratex->params.w,
        .height = ratex->params.h,
    };
    ra_gl_get_format(ratex->params.format, &par.iformat, &(GLenum){0}, &(GLenum){0});
    ra_gl_get_raw_tex(ra, ratex, &par.texture, &par.target);
    return pl_opengl_wrap(p->gpu, &par);
}

static void release_hwdec_planes(struct gpu_next_priv *p, struct pl_frame *frame)
{
    for (int n = 0; n < frame->num_planes; n++)
        pl_tex_destroy(p->gpu, &frame->planes[n].texture);
}

static void pre_render(struct gpu_next_priv *p)
{
    struct gl_api_priv *a = p->api_priv;
    if (!a->gl->es) // not a valid enable state on GLES
        a->gl->Disable(GL_FRAMEBUFFER_SRGB);
}

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct gpu_next_priv);
    struct gpu_next_priv *p = ctx->priv;
    p->global = ctx->global;
    p->log = ctx->log;
    struct gl_api_priv *a = talloc_zero(p, struct gl_api_priv);
    p->api_priv = a;
    p->wrap_hwdec_tex = wrap_hwdec_tex;
    p->release_hwdec_planes = release_hwdec_planes;
    p->pre_render = pre_render;

    const char *backend = getenv("MPV_LIBMPV_RENDER_BACKEND");
    if (!backend || strcmp(backend, "gpu-next") != 0)
        return MPV_ERROR_NOT_IMPLEMENTED;

    char *api = get_mpv_render_param(params, MPV_RENDER_PARAM_API_TYPE, NULL);
    if (!api || strcmp(api, MPV_RENDER_API_TYPE_OPENGL) != 0)
        return MPV_ERROR_NOT_IMPLEMENTED;

    a->context = talloc_zero(NULL, struct libmpv_gpu_context);
    *a->context = (struct libmpv_gpu_context){
        .global = ctx->global,
        .log = ctx->log,
        .fns = &libmpv_gpu_context_gl,
    };
    int err = a->context->fns->init(a->context, params);
    if (err < 0)
        return err;

    static const struct {
        const char *name;
        size_t size;
    } native_resource_map[] = {
        [MPV_RENDER_PARAM_X11_DISPLAY] = {"x11", 0},
        [MPV_RENDER_PARAM_WL_DISPLAY] = {"wl", 0},
        [MPV_RENDER_PARAM_DRM_DRAW_SURFACE_SIZE] =
            {"drm_draw_surface_size", sizeof (mpv_opengl_drm_draw_surface_size)},
        [MPV_RENDER_PARAM_DRM_DISPLAY_V2] =
            {"drm_params_v2", sizeof (mpv_opengl_drm_params_v2)},
    };
    for (int n = 0; params && params[n].type; n++) {
        if (params[n].type > 0 &&
            params[n].type < MP_ARRAY_SIZE(native_resource_map) &&
            native_resource_map[params[n].type].name)
        {
            void *data = params[n].data;
            size_t size = native_resource_map[params[n].type].size;
            if (size)
                data = talloc_memdup(p, data, size);
            ra_add_native_resource(a->context->ra_ctx->ra,
                                    native_resource_map[params[n].type].name, data);
        }
    }

    // Failures past this point return NOT_IMPLEMENTED, not UNSUPPORTED:
    // mpv_render_context_create() only advances to the next backend on
    // NOT_IMPLEMENTED, and an opt-in backend failing to bring up libplacebo
    // must not block the stock GL backend that would otherwise work.
    p->pllog = mppl_log_create(p, ctx->log);
    if (!p->pllog)
        return MPV_ERROR_NOT_IMPLEMENTED;

    a->gl = ra_gl_get(a->context->ra_ctx->ra);
    struct pl_opengl_params gl_params = {
        .debug = false,
        .allow_software = true,
        .get_proc_addr_ex = (void *)a->gl->get_fn,
        .proc_ctx = a->gl->fn_ctx,
    };
#if HAVE_EGL
    gl_params.egl_display = eglGetCurrentDisplay();
    gl_params.egl_context = eglGetCurrentContext();
#endif
    a->opengl = pl_opengl_create(p->pllog, &gl_params);
    if (!a->opengl) {
        MP_WARN(p, "gpu-next backend requested but libplacebo could not use "
                "this GL context; falling back to the standard backend.\n");
        return MPV_ERROR_NOT_IMPLEMENTED;
    }
    p->gpu = a->opengl->gpu;

    return lgn_common_init(ctx, a->context->ra_ctx);
}

static int get_target_size(struct render_backend *ctx, mpv_render_param *params,
                           int *out_w, int *out_h)
{
    struct gpu_next_priv *p = ctx->priv;
    struct gl_api_priv *a = p->api_priv;
    struct ra_tex *tex;
    int err = a->context->fns->wrap_fbo(a->context, params, &tex);
    if (err < 0)
        return err;
    *out_w = tex->params.w;
    *out_h = tex->params.h;
    return 0;
}

static int render(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame)
{
    struct gpu_next_priv *p = ctx->priv;
    struct gl_api_priv *a = p->api_priv;

    // mpv's wrap_fbo() only tracks an FBO id, not a real GL texture name --
    // wrapping that as a "raw tex" would silently target the default framebuffer.
    mpv_opengl_fbo *fbo = get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_FBO, NULL);
    if (!fbo)
        return MPV_ERROR_INVALID_PARAMETER;

    bool flip = GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_FLIP_Y, int, 0);
    struct pl_opengl_wrap_params wrap_params = {
        .framebuffer = fbo->fbo,
        .width = fbo->w,
        .height = fbo->h,
    };
    pl_tex target_tex = pl_opengl_wrap(p->gpu, &wrap_params);
    if (!target_tex)
        return MPV_ERROR_GENERIC;

    lgn_render_frame(ctx, params, frame, target_tex, flip, fbo->w, fbo->h);

    pl_tex_destroy(p->gpu, &target_tex);
    pl_gpu_flush(p->gpu);
    a->context->fns->done_frame(a->context, frame->display_synced);
    return 0;
}

static void destroy(struct render_backend *ctx)
{
    struct gpu_next_priv *p = ctx->priv;
    struct gl_api_priv *a = p->api_priv;

    lgn_common_uninit(ctx);

    pl_opengl_destroy(&a->opengl);
    if (p->pllog)
        pl_log_destroy(&p->pllog);

    if (a->context) {
        a->context->fns->destroy(a->context);
        talloc_free(a->context->priv);
        talloc_free(a->context);
    }
}

const struct render_backend_fns render_backend_gpu_next = {
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
