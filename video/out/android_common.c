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

#include <libavcodec/jni.h>
#include <android/native_window_jni.h>
#include <android/data_space.h>
#include <dlfcn.h>
#include <stdint.h>

#include "android_common.h"
#include "common/msg.h"
#include "misc/jni.h"
#include "options/m_config.h"
#include "video/mp_image.h"
#include "vo.h"

struct vo_android_state {
    struct mp_log *log;
    ANativeWindow *native_window;
    void *native_window_lib;
    int32_t (*set_buffers_dataspace)(ANativeWindow *, int32_t);
    int32_t last_dataspace;
};

static int32_t android_dataspace_from_params(
    const struct mp_image_params *params)
{
    if (!params)
        return ADATASPACE_UNKNOWN;

    // Android has no Dolby Vision dataspace. A decoded DV surface is a
    // BT.2020/PQ surface, while the decoder/consumer is responsible for
    // applying the profile-specific dynamic mapping.
    bool hdr_pq = params->color.transfer == PL_COLOR_TRC_PQ ||
                 params->repr.sys == PL_COLOR_SYSTEM_DOLBYVISION;
    bool hdr_hlg = params->color.transfer == PL_COLOR_TRC_HLG;
    bool limited = params->repr.levels != PL_COLOR_LEVELS_FULL;

    if (hdr_pq)
        return limited ? ADATASPACE_BT2020_ITU_PQ : ADATASPACE_BT2020_PQ;
    if (hdr_hlg)
        return limited ? ADATASPACE_BT2020_ITU_HLG : ADATASPACE_BT2020_HLG;

    if (params->color.primaries == PL_COLOR_PRIM_BT_2020 ||
        params->repr.sys == PL_COLOR_SYSTEM_BT_2020_NC ||
        params->repr.sys == PL_COLOR_SYSTEM_BT_2020_C)
    {
        // There is no named limited-range BT.2020 SDR value in the NDK
        // convenience constants, but the enum is explicitly bitfield based.
        return STANDARD_BT2020 | TRANSFER_SMPTE_170M |
               (limited ? RANGE_LIMITED : RANGE_FULL);
    }

    if (params->color.primaries == PL_COLOR_PRIM_BT_709 ||
        params->repr.sys == PL_COLOR_SYSTEM_BT_709)
    {
        return limited ? ADATASPACE_BT709 : ADATASPACE_SRGB;
    }

    return ADATASPACE_UNKNOWN;
}

bool vo_android_init(struct vo *vo)
{
    vo->android = talloc_zero(vo, struct vo_android_state);
    struct vo_android_state *ctx = vo->android;

    *ctx = (struct vo_android_state){
        .log = mp_log_new(ctx, vo->log, "android"),
        .last_dataspace = INT32_MIN,
    };

    // This API was added in API 28. Resolve it dynamically so Android builds
    // with a lower minSdk remain loadable and simply keep the platform default.
    ctx->native_window_lib = dlopen("libnativewindow.so",
                                    RTLD_NOW | RTLD_LOCAL);
    if (ctx->native_window_lib)
        ctx->set_buffers_dataspace = (void *)dlsym(
            ctx->native_window_lib, "ANativeWindow_setBuffersDataSpace");
    if (!ctx->set_buffers_dataspace)
        ctx->set_buffers_dataspace = (void *)dlsym(
            RTLD_DEFAULT, "ANativeWindow_setBuffersDataSpace");

    JNIEnv *env = MP_JNI_GET_ENV(ctx);
    if (!env) {
        MP_FATAL(ctx, "Could not attach java VM.\n");
        goto fail;
    }

    if (vo->opts->WinID == 0 || vo->opts->WinID == -1) {
        MP_FATAL(ctx, "Missing surface pointer\n");
        goto fail;
    }
    jobject surface = (jobject)(intptr_t)vo->opts->WinID;
    ctx->native_window = ANativeWindow_fromSurface(env, surface);
    if (!ctx->native_window) {
        MP_FATAL(ctx, "Failed to create ANativeWindow\n");
        goto fail;
    }

    return true;
fail:
    if (ctx->native_window)
        ANativeWindow_release(ctx->native_window);
    if (ctx->native_window_lib)
        dlclose(ctx->native_window_lib);
    talloc_free(ctx);
    vo->android = NULL;
    return false;
}

void vo_android_uninit(struct vo *vo)
{
    struct vo_android_state *ctx = vo->android;
    if (!ctx)
        return;

    if (ctx->native_window)
        ANativeWindow_release(ctx->native_window);
    if (ctx->native_window_lib)
        dlclose(ctx->native_window_lib);

    talloc_free(ctx);
    vo->android = NULL;
}

ANativeWindow *vo_android_native_window(struct vo *vo)
{
    struct vo_android_state *ctx = vo->android;
    return ctx->native_window;
}

bool vo_android_surface_size(struct vo *vo, int *out_w, int *out_h)
{
    struct vo_android_state *ctx = vo->android;

    int w = vo->opts->android_surface_size.w,
        h = vo->opts->android_surface_size.h;
    if (!w)
        w = ANativeWindow_getWidth(ctx->native_window);
    if (!h)
        h = ANativeWindow_getHeight(ctx->native_window);

    if (w <= 0 || h <= 0) {
        MP_ERR(ctx, "Failed to get height and width.\n");
        return false;
    }
    *out_w = w;
    *out_h = h;
    return true;
}

void vo_android_set_buffers_dataspace(struct vo *vo,
                                      const struct mp_image_params *params)
{
    struct vo_android_state *ctx = vo->android;
    if (!ctx || !ctx->native_window || !ctx->set_buffers_dataspace)
        return;

    int32_t dataspace = android_dataspace_from_params(params);
    if (dataspace == ctx->last_dataspace)
        return;

    int32_t ret = ctx->set_buffers_dataspace(ctx->native_window, dataspace);
    if (ret < 0) {
        MP_VERBOSE(ctx, "Failed to set Android buffer dataspace %#x: %d\n",
                   dataspace, ret);
        return;
    }

    ctx->last_dataspace = dataspace;
    MP_VERBOSE(ctx, "Android buffer dataspace set to %#x (%s)\n", dataspace,
               dataspace == ADATASPACE_BT2020_ITU_PQ ||
                       dataspace == ADATASPACE_BT2020_PQ ? "BT.2020/PQ" :
               dataspace == ADATASPACE_BT2020_ITU_HLG ||
                       dataspace == ADATASPACE_BT2020_HLG ? "BT.2020/HLG" :
               dataspace == ADATASPACE_UNKNOWN ? "unknown" : "SDR");
}
