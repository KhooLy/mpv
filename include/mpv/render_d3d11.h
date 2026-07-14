/* Copyright (C) 2026 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MPV_CLIENT_API_RENDER_D3D11_H_
#define MPV_CLIENT_API_RENDER_D3D11_H_

#include <d3d11.h>

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Direct3D 11 backend
 * --------------------
 *
 * This backend requires the caller to have created its own ID3D11Device, and
 * to hand it to mpv via MPV_RENDER_PARAM_D3D11_INIT_PARAMS. Unlike the OpenGL
 * backend, mpv does not create or own the device.
 *
 * Use mpv_render_context_create() with MPV_RENDER_PARAM_API_TYPE set to
 * MPV_RENDER_API_TYPE_D3D11, and MPV_RENDER_PARAM_D3D11_INIT_PARAMS provided.
 *
 * Call mpv_render_context_render() with MPV_RENDER_PARAM_D3D11_TARGET set to
 * render the video frame into a caller-owned ID3D11Texture2D.
 *
 * mpv issues all Direct3D commands through the immediate context of the
 * caller-provided device. Since a device's immediate context serializes
 * command submission in issue order, the caller does not need any additional
 * synchronization around the target texture beyond the ordering it already
 * gets from submitting its own work on the same device before and after
 * calling mpv_render_context_render().
 *
 * This backend additionally requires that
 * MPV_LIBMPV_RENDER_BACKEND=gpu-next is set in the environment before
 * mpv_render_context_create() is called, same as the OpenGL and Vulkan
 * gpu-next backends -- it is opt-in and does not affect any other libmpv
 * embedder.
 */

/**
 * For initializing the mpv D3D11 state via MPV_RENDER_PARAM_D3D11_INIT_PARAMS.
 *
 * mpv does not create or own this object. It must outlive the render context,
 * and the caller remains responsible for releasing it after
 * mpv_render_context_free() returns.
 */
typedef struct mpv_d3d11_init_params {
    /**
     * Caller-owned Direct3D 11 device.
     */
    ID3D11Device *device;
} mpv_d3d11_init_params;

/**
 * For MPV_RENDER_PARAM_D3D11_TARGET.
 */
typedef struct mpv_d3d11_target {
    /**
     * Caller-owned target texture. Must have been created with
     * D3D11_BIND_RENDER_TARGET set, on the same ID3D11Device passed via
     * mpv_d3d11_init_params, and must not be mipmapped or multisampled.
     */
    ID3D11Texture2D *tex;
    DXGI_FORMAT format;
    int w, h;
} mpv_d3d11_target;

#ifdef __cplusplus
}
#endif

#endif
