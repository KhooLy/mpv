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
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/native_window_jni.h>
#include <media/NdkImageReader.h>
#include <libavcodec/mediacodec.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>
#include <libplacebo/colorspace.h>

#include "common/common.h"
#include "common/msg.h"
#include "options/options.h"
#include "misc/jni.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "video/hwdec.h"
#include "video/decode/dovi_rpu.h"
#include "video/mp_image.h"
#include "android_common.h"
#include "android_fel.h"
#include "vo.h"

#ifndef EGL_GL_COLORSPACE_BT2020_PQ_EXT
#define EGL_GL_COLORSPACE_BT2020_PQ_EXT 0x3340
#endif
#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif
#ifndef EGL_SMPTE2086_DISPLAY_PRIMARY_RX_EXT
#define EGL_SMPTE2086_DISPLAY_PRIMARY_RX_EXT 0x3341
#define EGL_SMPTE2086_DISPLAY_PRIMARY_RY_EXT 0x3342
#define EGL_SMPTE2086_DISPLAY_PRIMARY_GX_EXT 0x3343
#define EGL_SMPTE2086_DISPLAY_PRIMARY_GY_EXT 0x3344
#define EGL_SMPTE2086_DISPLAY_PRIMARY_BX_EXT 0x3345
#define EGL_SMPTE2086_DISPLAY_PRIMARY_BY_EXT 0x3346
#define EGL_SMPTE2086_WHITE_POINT_X_EXT 0x3347
#define EGL_SMPTE2086_WHITE_POINT_Y_EXT 0x3348
#define EGL_SMPTE2086_MAX_LUMINANCE_EXT 0x3349
#define EGL_SMPTE2086_MIN_LUMINANCE_EXT 0x334A
#define EGL_METADATA_SCALING_EXT 50000
#endif
#ifndef EGL_CTA861_3_MAX_CONTENT_LIGHT_LEVEL_EXT
#define EGL_CTA861_3_MAX_CONTENT_LIGHT_LEVEL_EXT 0x3360
#define EGL_CTA861_3_MAX_FRAME_AVERAGE_LEVEL_EXT 0x3361
#endif

#define MAX_IMAGES 5
#define MAX_CACHE 24
#define MAX_MMR 48
#define STAT_W 256
#define STAT_H 144
#define STAT_BINS 1024

struct cached_tex {
    AHardwareBuffer *buf;
    EGLImageKHR image;
    GLuint tex;
};

struct layer {
    struct android_fel *f;
    const char *name;
    struct mp_hwdec_ctx hwctx;
    AImageReader *reader;
    jobject surface;
    mp_mutex lock;
    mp_cond cond;
    bool available;
    AImage *hist[3];
    struct cached_tex cache[MAX_CACHE];
    int num_cache;
    GLuint tex;
    float crop[4];
    int w, h;
};

struct program {
    GLuint id;
    GLint bl, el, blc, elc, pivots, coeffs, mmr, lo, hi, act;
    GLint ycc, ycc_off, lms, nlq_off, nlq_slope, nlq_thr, tm, trim;
};

struct android_fel {
    struct vo *vo;
    struct mp_log *log;
    bool failed, registered;

    EGLDisplay dpy;
    EGLConfig config;
    EGLContext ctx;
    EGLSurface surface;
    bool hdr_md;
    struct pl_hdr_metadata hdr;
    float hdr10p_peak, hdr10p_avg;
    float display_peak;
    bool display_hdr10p;

    GLuint stat_fbo, stat_tex, stat_pbo[2];
    int stat_scene[2];
    bool stat_pending[2];
    int stat_idx;
    int scene, since_sent;
    uint64_t hist[STAT_BINS];
    uint64_t hist_n;
    float nits[STAT_BINS];

    EGLImageKHR (*CreateImageKHR)(EGLDisplay, EGLContext, EGLenum,
                                  EGLClientBuffer, const EGLint *);
    EGLBoolean (*DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    EGLClientBuffer (*GetNativeClientBuffer)(const struct AHardwareBuffer *);
    void (*ImageTargetTexture)(GLenum, GLeglImageOES);
    EGLBoolean (*PresentationTime)(EGLDisplay, EGLSurface, EGLnsecsANDROID);

    struct program prog[2][2];
    struct layer bl, el;
};

static const char vert_src[] =
    "#version 300 es\n"
    "out vec2 pos;\n"
    "void main() {\n"
    "    vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 2.0;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "    pos = vec2(p.x, 1.0 - p.y);\n"
    "}\n";

static const char frag_src[] =
    "#extension GL_EXT_YUV_target : require\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "in vec2 pos;\n"
    "out vec4 frag;\n"
    "uniform __samplerExternal2DY2YEXT bl;\n"
    "uniform vec4 blc;\n"
    "uniform mat3 ycc;\n"
    "uniform vec3 ycc_off;\n"
    "#ifdef DV\n"
    "uniform vec4 pivots[6];\n"
    "uniform vec4 coeffs[24];\n"
    "uniform vec4 mmr[48];\n"
    "uniform vec3 lo, hi, act;\n"
    "uniform mat3 lms;\n"
    "uniform vec4 tm, trim;\n"
    "const float M1 = 0.1593017578125, M2 = 78.84375;\n"
    "const float C1 = 0.8359375, C2 = 18.8515625, C3 = 18.6875;\n"
    "float pq_enc(float x) {\n"
    "    x = pow(max(x, 0.0), M1);\n"
    "    return pow((C1 + C2 * x) / (1.0 + C3 * x), M2);\n"
    "}\n"
    "float pq_dec(float x) {\n"
    "    x = pow(max(x, 0.0), 1.0 / M2);\n"
    "    return pow(max(x - C1, 0.0) / (C2 - C3 * x), 1.0 / M1);\n"
    "}\n"
    "vec3 tonemap(vec3 c) {\n"
    "    float y = dot(c, vec3(0.2627, 0.6780, 0.0593));\n"
    "    float e = clamp((pq_enc(y) - tm.x) / (tm.y - tm.x), 0.0, 1.0);\n"
    "    float maxl = (tm.z - tm.x) / (tm.y - tm.x);\n"
    "    float ks = max(1.5 * maxl - 0.5, 0.0);\n"
    "    if (e > ks) {\n"
    "        float t = (e - ks) / (1.0 - ks), t2 = t * t, t3 = t2 * t;\n"
    "        e = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) +\n"
    "            (3.0 * t2 - 2.0 * t3) * maxl;\n"
    "    }\n"
    "    float x = (e * (tm.y - tm.x) + tm.x) / tm.z;\n"
    "    x = pow(clamp(x * trim.x + trim.y, 0.0, 1.0), trim.z);\n"
    "    float y2 = pq_dec(x * tm.z);\n"
    "    c = y > 0.0 ? c * (y2 / y) : vec3(0.0);\n"
    "    c = y2 + (c - y2) * trim.w;\n"
    "    float peak = pq_dec(tm.z), m = max(max(c.r, c.g), c.b);\n"
    "    if (m > peak)\n"
    "        c = mix(vec3(y2), c, (peak - y2) / (m - y2));\n"
    "    return max(c, 0.0);\n"
    "}\n"
    "float reshape(int c, vec3 sig) {\n"
    "    float s = sig[c];\n"
    "    int i = int(dot(vec4(greaterThanEqual(vec4(s), pivots[2 * c])), vec4(1.0)) +\n"
    "                dot(vec4(greaterThanEqual(vec4(s), pivots[2 * c + 1])), vec4(1.0)));\n"
    "    vec4 k = coeffs[8 * c + i];\n"
    "    if (k.w == 0.0)\n"
    "        return (k.z * s + k.y) * s + k.x;\n"
    "    int m = int(k.y);\n"
    "    vec4 x = vec4(sig.xxy * sig.yzz, sig.x * sig.y * sig.z);\n"
    "    float r = k.x + dot(mmr[m].xyz, sig) + dot(mmr[m + 1], x);\n"
    "    if (k.w >= 2.0) {\n"
    "        vec3 s2 = sig * sig;\n"
    "        vec4 x2 = x * x;\n"
    "        r += dot(mmr[m + 2].xyz, s2) + dot(mmr[m + 3], x2);\n"
    "        if (k.w >= 3.0)\n"
    "            r += dot(mmr[m + 4].xyz, s2 * sig) + dot(mmr[m + 5], x2 * x);\n"
    "    }\n"
    "    return r;\n"
    "}\n"
    "#endif\n"
    "#ifdef EL\n"
    "uniform __samplerExternal2DY2YEXT el;\n"
    "uniform vec4 elc;\n"
    "uniform vec3 nlq_off, nlq_slope, nlq_thr;\n"
    "#endif\n"
    "void main() {\n"
    "    vec3 c = texture(bl, blc.xy + pos * blc.zw).rgb;\n"
    "#ifdef DV\n"
    "    vec3 sig = clamp(c, 0.0, 1.0);\n"
    "    vec3 r = vec3(reshape(0, sig), reshape(1, sig), reshape(2, sig));\n"
    "    c = mix(c, clamp(r, lo, hi), act);\n"
    "#endif\n"
    "#ifdef EL\n"
    "    vec3 e = texture(el, elc.xy + pos * elc.zw).rgb - nlq_off;\n"
    "    c += sign(e) * (abs(e) * nlq_slope + nlq_thr);\n"
    "#endif\n"
    "    c = ycc * (c - ycc_off);\n"
    "#ifdef DV\n"
    "    c = pow(max(c, 0.0), vec3(1.0 / M2));\n"
    "    c = max(c - C1, 0.0) / (C2 - C3 * c);\n"
    "    c = lms * pow(c, vec3(1.0 / M1));\n"
    "    if (tm.w > 0.0)\n"
    "        c = tonemap(c);\n"
    "    c = pow(max(c, 0.0), vec3(M1));\n"
    "    c = pow((C1 + C2 * c) / (1.0 + C3 * c), vec3(M2));\n"
    "#endif\n"
    "    frag = vec4(c, 1.0);\n"
    "}\n";

static void image_available(void *ctx, AImageReader *reader)
{
    struct layer *l = ctx;
    mp_mutex_lock(&l->lock);
    l->available = true;
    mp_cond_signal(&l->cond);
    mp_mutex_unlock(&l->lock);
}

static bool layer_init(struct android_fel *f, struct layer *l, const char *name)
{
    l->f = f;
    l->name = name;
    mp_mutex_init(&l->lock);
    mp_cond_init(&l->cond);

    if (AImageReader_newWithUsage(16, 16, AIMAGE_FORMAT_PRIVATE,
                                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
                                  MAX_IMAGES, &l->reader) != AMEDIA_OK)
        return false;
    AImageReader_ImageListener listener = {
        .context = l,
        .onImageAvailable = image_available,
    };
    AImageReader_setImageListener(l->reader, &listener);

    ANativeWindow *win;
    if (AImageReader_getWindow(l->reader, &win) != AMEDIA_OK)
        return false;
    JNIEnv *env = MP_JNI_GET_ENV(f);
    if (!env)
        return false;
    jobject surface = ANativeWindow_toSurface(env, win);
    if (!surface)
        return false;
    l->surface = (*env)->NewGlobalRef(env, surface);
    (*env)->DeleteLocalRef(env, surface);

    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!ref)
        return false;
    AVHWDeviceContext *dev = (void *)ref->data;
    ((AVMediaCodecDeviceContext *)dev->hwctx)->surface = l->surface;
    if (av_hwdevice_ctx_init(ref) < 0) {
        av_buffer_unref(&ref);
        return false;
    }
    l->hwctx = (struct mp_hwdec_ctx){
        .driver_name = name,
        .av_device_ref = ref,
        .hw_imgfmt = IMGFMT_MEDIACODEC,
    };
    return true;
}

static void cache_flush(struct android_fel *f, struct layer *l)
{
    for (int n = 0; n < l->num_cache; n++) {
        glDeleteTextures(1, &l->cache[n].tex);
        f->DestroyImageKHR(f->dpy, l->cache[n].image);
    }
    l->num_cache = 0;
}

static void layer_uninit(struct android_fel *f, struct layer *l)
{
    if (!l->f)
        return;
    if (l->hwctx.av_device_ref) {
        hwdec_devices_remove(f->vo->hwdec_devs, &l->hwctx);
        av_buffer_unref(&l->hwctx.av_device_ref);
    }
    if (f->ctx)
        cache_flush(f, l);
    for (int n = 0; n < MP_ARRAY_SIZE(l->hist); n++) {
        if (l->hist[n])
            AImage_delete(l->hist[n]);
        l->hist[n] = NULL;
    }
    if (l->surface) {
        JNIEnv *env = MP_JNI_GET_ENV(f);
        if (env)
            (*env)->DeleteGlobalRef(env, l->surface);
        l->surface = NULL;
    }
    if (l->reader) {
        AImageReader_setImageListener(l->reader, NULL);
        AImageReader_delete(l->reader);
        l->reader = NULL;
    }
    mp_mutex_destroy(&l->lock);
    mp_cond_destroy(&l->cond);
    l->f = NULL;
}

static GLuint compile(struct android_fel *f, GLenum type, const char *head,
                      const char *src)
{
    GLuint s = glCreateShader(type);
    const char *parts[] = {head, src};
    glShaderSource(s, 2, parts, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char msg[1024] = {0};
        glGetShaderInfoLog(s, sizeof(msg) - 1, NULL, msg);
        MP_ERR(f, "FEL shader compile failed: %s\n", msg);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool build_program(struct android_fel *f, struct program *p, bool dv, bool el)
{
    char head[128];
    snprintf(head, sizeof(head), "#version 300 es\n%s%s",
             dv ? "#define DV\n" : "", el ? "#define EL\n" : "");
    GLuint vs = compile(f, GL_VERTEX_SHADER, "", vert_src);
    GLuint fs = compile(f, GL_FRAGMENT_SHADER, head, frag_src);
    if (!vs || !fs)
        return false;
    p->id = glCreateProgram();
    glAttachShader(p->id, vs);
    glAttachShader(p->id, fs);
    glLinkProgram(p->id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p->id, GL_LINK_STATUS, &ok);
    if (!ok) {
        MP_ERR(f, "FEL shader link failed\n");
        return false;
    }
#define LOC(x) p->x = glGetUniformLocation(p->id, #x)
    LOC(bl); LOC(el); LOC(blc); LOC(elc); LOC(pivots); LOC(coeffs); LOC(mmr);
    LOC(lo); LOC(hi); LOC(act); LOC(ycc); LOC(ycc_off); LOC(lms);
    LOC(nlq_off); LOC(nlq_slope); LOC(nlq_thr); LOC(tm); LOC(trim);
#undef LOC
    glUseProgram(p->id);
    glUniform1i(p->bl, 0);
    glUniform1i(p->el, 1);
    return true;
}

static bool has_ext(const char *exts, const char *ext)
{
    return exts && strstr(exts, ext);
}

static bool egl_init(struct android_fel *f)
{
    f->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (f->dpy == EGL_NO_DISPLAY || !eglInitialize(f->dpy, NULL, NULL))
        return false;
    const char *exts = eglQueryString(f->dpy, EGL_EXTENSIONS);
    if (!has_ext(exts, "EGL_EXT_gl_colorspace_bt2020_pq") ||
        !has_ext(exts, "EGL_ANDROID_image_native_buffer") ||
        !has_ext(exts, "EGL_ANDROID_get_native_client_buffer") ||
        !has_ext(exts, "EGL_ANDROID_presentation_time"))
    {
        MP_VERBOSE(f, "EGL lacks the extensions needed for FEL compose\n");
        return false;
    }
    f->hdr_md = has_ext(exts, "EGL_EXT_surface_SMPTE2086_metadata") &&
                has_ext(exts, "EGL_EXT_surface_CTA861_3_metadata");

    const EGLint attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 10, EGL_GREEN_SIZE, 10, EGL_BLUE_SIZE, 10, EGL_ALPHA_SIZE, 2,
        EGL_NONE,
    };
    EGLint num = 0;
    if (!eglChooseConfig(f->dpy, attrs, &f->config, 1, &num) || num < 1) {
        MP_VERBOSE(f, "No 10-bit EGL config\n");
        return false;
    }
    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    f->ctx = eglCreateContext(f->dpy, f->config, EGL_NO_CONTEXT, ctx_attrs);
    if (f->ctx == EGL_NO_CONTEXT)
        return false;
    if (!eglMakeCurrent(f->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, f->ctx))
        return false;

    if (!has_ext((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_YUV_target")) {
        MP_VERBOSE(f, "GLES lacks GL_EXT_YUV_target\n");
        return false;
    }

    f->CreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
    f->DestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
    f->GetNativeClientBuffer = (void *)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    f->ImageTargetTexture = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    f->PresentationTime = (void *)eglGetProcAddress("eglPresentationTimeANDROID");
    if (!f->CreateImageKHR || !f->DestroyImageKHR || !f->GetNativeClientBuffer ||
        !f->ImageTargetTexture || !f->PresentationTime)
        return false;

    for (int dv = 0; dv < 2; dv++) {
        for (int el = 0; el <= dv; el++) {
            if (!build_program(f, &f->prog[dv][el], dv, el))
                return false;
        }
    }

    glGenTextures(1, &f->stat_tex);
    glBindTexture(GL_TEXTURE_2D, f->stat_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB10_A2, STAT_W, STAT_H);
    glGenFramebuffers(1, &f->stat_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, f->stat_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           f->stat_tex, 0);
    bool fbo_ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo_ok) {
        glGenBuffers(2, f->stat_pbo);
        for (int i = 0; i < 2; i++) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, f->stat_pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, STAT_W * STAT_H * 4, NULL, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    } else {
        MP_VERBOSE(f, "Brightness measurement unavailable\n");
    }
    for (int i = 0; i < STAT_BINS; i++)
        f->nits[i] = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, i / (float)(STAT_BINS - 1));
    return true;
}

static bool setup(struct android_fel *f)
{
    if (f->registered)
        return true;
    if (f->failed)
        return false;
    if (!egl_init(f) || !layer_init(f, &f->bl, "mediacodec_fel_bl") ||
        !layer_init(f, &f->el, "mediacodec_fel_el"))
    {
        MP_WARN(f, "Dolby Vision FEL compose unavailable\n");
        f->failed = true;
        return false;
    }
    hwdec_devices_add(f->vo->hwdec_devs, &f->bl.hwctx);
    hwdec_devices_add(f->vo->hwdec_devs, &f->el.hwctx);
    f->registered = true;
    return true;
}

static bool attach(struct android_fel *f)
{
    if (f->surface != EGL_NO_SURFACE)
        return true;
    ANativeWindow *win = vo_android_native_window(f->vo);
    ANativeWindow_setBuffersGeometry(win, 0, 0, 0);
    const EGLint attrs[] = {
        EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_BT2020_PQ_EXT,
        EGL_NONE,
    };
    f->surface = eglCreateWindowSurface(f->dpy, f->config, win, attrs);
    if (f->surface == EGL_NO_SURFACE) {
        MP_VERBOSE(f, "eglCreateWindowSurface failed: 0x%x\n", eglGetError());
        return false;
    }
    eglMakeCurrent(f->dpy, f->surface, f->surface, f->ctx);
    eglSwapInterval(f->dpy, 0);
    f->hdr = (struct pl_hdr_metadata){0};
    f->hdr10p_peak = f->hdr10p_avg = -1;
    f->stat_pending[0] = f->stat_pending[1] = false;
    int hdr_types = mp_jni_display_hdr_types(f->log, &f->display_peak);
    f->display_hdr10p = hdr_types & MP_JNI_HDR_HDR10_PLUS;
    if (!(hdr_types & MP_JNI_HDR_HDR10) || f->display_peak <= 0)
        f->display_peak = 203;
    MP_VERBOSE(f, "Display peak %.0f nits\n", f->display_peak);
    MP_VERBOSE(f, "Composing Dolby Vision FEL on the GPU\n");
    return true;
}

static void detach(struct android_fel *f)
{
    if (f->surface == EGL_NO_SURFACE)
        return;
    eglMakeCurrent(f->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, f->ctx);
    eglDestroySurface(f->dpy, f->surface);
    f->surface = EGL_NO_SURFACE;
}

struct android_fel *android_fel_create(struct vo *vo)
{
    struct android_fel *f = talloc_zero(NULL, struct android_fel);
    f->vo = vo;
    f->log = mp_log_new(f, vo->log, "fel");
    f->dpy = EGL_NO_DISPLAY;
    f->ctx = EGL_NO_CONTEXT;
    f->surface = EGL_NO_SURFACE;
    return f;
}

void android_fel_destroy(struct android_fel *f)
{
    if (!f)
        return;
    detach(f);
    layer_uninit(f, &f->bl);
    layer_uninit(f, &f->el);
    if (f->ctx != EGL_NO_CONTEXT) {
        for (int dv = 0; dv < 2; dv++) {
            for (int el = 0; el < 2; el++) {
                if (f->prog[dv][el].id)
                    glDeleteProgram(f->prog[dv][el].id);
            }
        }
        eglMakeCurrent(f->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(f->dpy, f->ctx);
    }
    if (f->dpy != EGL_NO_DISPLAY)
        eglTerminate(f->dpy);
    talloc_free(f);
}

void android_fel_select(struct android_fel *f, bool on)
{
    if (on && setup(f)) {
        attach(f);
    } else if (!on) {
        detach(f);
    }
}

bool android_fel_active(struct android_fel *f)
{
    return f && f->registered && f->surface != EGL_NO_SURFACE;
}

static GLuint cached_tex(struct android_fel *f, struct layer *l, AHardwareBuffer *buf)
{
    for (int n = 0; n < l->num_cache; n++) {
        if (l->cache[n].buf == buf)
            return l->cache[n].tex;
    }
    if (l->num_cache == MAX_CACHE)
        cache_flush(f, l);

    EGLClientBuffer cb = f->GetNativeClientBuffer(buf);
    const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR image = cb ? f->CreateImageKHR(f->dpy, EGL_NO_CONTEXT,
                                               EGL_NATIVE_BUFFER_ANDROID, cb, attrs)
                           : EGL_NO_IMAGE_KHR;
    if (image == EGL_NO_IMAGE_KHR)
        return 0;
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f->ImageTargetTexture(GL_TEXTURE_EXTERNAL_OES, image);
    l->cache[l->num_cache++] = (struct cached_tex){buf, image, tex};
    return tex;
}

static bool layer_acquire(struct android_fel *f, struct layer *l)
{
    mp_mutex_lock(&l->lock);
    if (!l->available)
        mp_cond_timedwait(&l->cond, &l->lock, MP_TIME_MS_TO_NS(100));
    bool got = l->available;
    l->available = false;
    mp_mutex_unlock(&l->lock);
    if (!got)
        MP_WARN(f, "Timed out waiting for the %s frame\n", l->name);

    AImage *img = NULL;
    if (AImageReader_acquireLatestImage(l->reader, &img) != AMEDIA_OK || !img)
        return false;

    AHardwareBuffer *buf = NULL;
    AImageCropRect crop;
    if (AImage_getHardwareBuffer(img, &buf) != AMEDIA_OK || !buf ||
        AImage_getCropRect(img, &crop) != AMEDIA_OK)
    {
        AImage_delete(img);
        return false;
    }
    GLuint tex = cached_tex(f, l, buf);
    if (!tex) {
        AImage_delete(img);
        return false;
    }

    int last = MP_ARRAY_SIZE(l->hist) - 1;
    if (l->hist[last])
        AImage_delete(l->hist[last]);
    memmove(&l->hist[1], &l->hist[0], last * sizeof(l->hist[0]));
    l->hist[0] = img;

    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(buf, &desc);
    l->tex = tex;
    l->w = crop.right - crop.left;
    l->h = crop.bottom - crop.top;
    l->crop[0] = crop.left / (float)desc.width;
    l->crop[1] = crop.top / (float)desc.height;
    l->crop[2] = l->w / (float)desc.width;
    l->crop[3] = l->h / (float)desc.height;
    return true;
}

static void mat_mul(float out[3][3], const float a[3][3], const float b[3][3])
{
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        }
    }
}

static void upload_dovi(struct android_fel *f, struct program *p,
                        const struct pl_dovi_metadata *d)
{
    float pivots[6][4], coeffs[24][4] = {0}, mmr[MAX_MMR][4] = {0};
    float lo[3], hi[3], active[3];
    int midx = 0;
    for (int c = 0; c < 3; c++) {
        const struct pl_reshape_data *comp = &d->comp[c];
        for (int i = 0; i < 8; i++)
            pivots[2 * c + i / 4][i % 4] = 1e9f;
        active[c] = comp->num_pivots >= 2;
        lo[c] = 0;
        hi[c] = 1;
        if (!active[c])
            continue;
        for (int i = 0; i < comp->num_pivots - 2; i++)
            pivots[2 * c + i / 4][i % 4] = comp->pivots[i + 1];
        lo[c] = comp->pivots[0];
        hi[c] = comp->pivots[comp->num_pivots - 1];
        for (int i = 0; i < comp->num_pivots - 1; i++) {
            float *k = coeffs[8 * c + i];
            if (comp->method[i] == 0) {
                memcpy(k, comp->poly_coeffs[i], 3 * sizeof(float));
                continue;
            }
            int order = comp->mmr_order[i];
            if (midx + 2 * order > MAX_MMR) {
                MP_WARN(f, "Too many MMR coefficients\n");
                break;
            }
            k[0] = comp->mmr_constant[i];
            k[1] = midx;
            k[3] = order;
            for (int j = 0; j < order; j++) {
                const float *w = comp->mmr_coeffs[i][j];
                memcpy(mmr[midx], w, 3 * sizeof(float));
                memcpy(mmr[midx + 1], w + 3, 4 * sizeof(float));
                midx += 2;
            }
        }
    }
    glUniform4fv(p->pivots, 6, &pivots[0][0]);
    glUniform4fv(p->coeffs, 24, &coeffs[0][0]);
    if (midx)
        glUniform4fv(p->mmr, midx, &mmr[0][0]);
    glUniform3fv(p->lo, 1, lo);
    glUniform3fv(p->hi, 1, hi);
    glUniform3fv(p->act, 1, active);

    static const float lms2rgb[3][3] = {
        { 3.06441879, -2.16597676,  0.10155818},
        {-0.65612108,  1.78554118, -0.12943749},
        { 0.01736321, -0.04725154,  1.03004253},
    };
    float lms[3][3];
    mat_mul(lms, lms2rgb, d->linear.m);
    glUniformMatrix3fv(p->lms, 1, GL_TRUE, &lms[0][0]);
    glUniformMatrix3fv(p->ycc, 1, GL_TRUE, &d->nonlinear.m[0][0]);
    float off[3];
    for (int i = 0; i < 3; i++)
        off[i] = d->nonlinear_offset[i] * (1024.0f / 1023.0f);
    glUniform3fv(p->ycc_off, 1, off);

    if (p->nlq_off >= 0) {
        float o[3], s[3], t[3];
        for (int c = 0; c < 3; c++) {
            o[c] = d->nlq[c].offset;
            s[c] = d->nlq[c].deadzone_slope;
            t[c] = d->nlq[c].deadzone_threshold;
        }
        glUniform3fv(p->nlq_off, 1, o);
        glUniform3fv(p->nlq_slope, 1, s);
        glUniform3fv(p->nlq_thr, 1, t);
    }
}

static void upload_tonemap(struct android_fel *f, struct program *p,
                           struct mp_image *img)
{
    const struct pl_hdr_metadata *hdr = &img->params.color.hdr;
    float dst = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, f->display_peak);
    float src = hdr->max_pq_y > 0 ? hdr->max_pq_y
              : pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, hdr->max_luma > 0 ? hdr->max_luma : 1000);
    int opt = f->vo->opts->android_dovi_tonemap;
    bool on = opt < 0 ? !f->display_hdr10p : opt;
    glUniform4f(p->tm, 0, MPMAX(src, 0.01f), dst, on);

    const AVDOVIDmLevel2 *best = NULL;
    for (int n = 0; on && n < img->num_ff_side_data; n++) {
        if (img->ff_side_data[n].type != AV_FRAME_DATA_DOVI_METADATA)
            continue;
        const AVDOVIMetadata *md = (void *)img->ff_side_data[n].buf->data;
        for (int i = 0; i < md->num_ext_blocks; i++) {
            const AVDOVIDmData *ext = av_dovi_get_ext(md, i);
            if (ext->level != 2)
                continue;
            if (!best || fabsf(ext->l2.target_max_pq / 4095.0f - dst) <
                         fabsf(best->target_max_pq / 4095.0f - dst))
                best = &ext->l2;
        }
    }
    if (best) {
        glUniform4f(p->trim, best->trim_slope / 4096.0f + 0.5f,
                    best->trim_offset / 4096.0f - 0.5f,
                    best->trim_power / 4096.0f + 0.5f,
                    best->trim_saturation_gain / 4096.0f + 0.5f);
    } else {
        glUniform4f(p->trim, 1, 0, 1, 1);
    }
}

static void upload_bt2020(struct program *p)
{
    const float ys = 1023.0f / 876, cs = 1023.0f / 896;
    const float m[3][3] = {
        {ys, 0, 1.4746f * cs},
        {ys, -0.16455f * cs, -0.57135f * cs},
        {ys, 1.8814f * cs, 0},
    };
    const float off[3] = {64 / 1023.0f, 512 / 1023.0f, 512 / 1023.0f};
    glUniformMatrix3fv(p->ycc, 1, GL_TRUE, &m[0][0]);
    glUniform3fv(p->ycc_off, 1, off);
}

static void update_hdr(struct android_fel *f, const struct pl_hdr_metadata *hdr)
{
    if (!f->hdr_md || !memcmp(hdr, &f->hdr, sizeof(*hdr)))
        return;
    f->hdr = *hdr;
    const float s = EGL_METADATA_SCALING_EXT;
    const struct pl_raw_primaries *pr = &hdr->prim;
    if (pr->red.x > 0) {
        eglSurfaceAttrib(f->dpy, f->surface, EGL_SMPTE2086_DISPLAY_PRIMARY_RX_EXT, pr->red.x * s);
        eglSurfaceAttrib(f->dpy, f->surface, EGL_SMPTE2086_DISPLAY_PRIMARY_RY_EXT, pr->red.y * s);
        eglSurfaceAttrib(f->dpy, f->surface, EGL_SMPTE2086_DISPLAY_PRIMARY_GX_EXT, pr->green.x * s);
        eglSurfaceAttrib(f->dpy, f->surface, EGL_SMPTE2086_DISPLAY_PRIMARY_GY_EXT, pr->green.y * s);
        eglSurfaceAttrib(f->dpy, f->surface, EGL_SMPTE2086_DISPLAY_PRIMARY_BX_EXT, pr->blue.x * s);
        eglSurfaceAttrib(f->dpy, f->surface, EGL_SMPTE2086_DISPLAY_PRIMARY_BY_EXT, pr->blue.y * s);
        eglSurfaceAttrib(f->dpy, f->surface, EGL_SMPTE2086_WHITE_POINT_X_EXT, pr->white.x * s);
        eglSurfaceAttrib(f->dpy, f->surface, EGL_SMPTE2086_WHITE_POINT_Y_EXT, pr->white.y * s);
    }
    if (hdr->max_luma > 0) {
        eglSurfaceAttrib(f->dpy, f->surface, EGL_SMPTE2086_MAX_LUMINANCE_EXT, hdr->max_luma * s);
        eglSurfaceAttrib(f->dpy, f->surface, EGL_SMPTE2086_MIN_LUMINANCE_EXT, hdr->min_luma * s);
    }
    if (hdr->max_cll > 0) {
        eglSurfaceAttrib(f->dpy, f->surface, EGL_CTA861_3_MAX_CONTENT_LIGHT_LEVEL_EXT, hdr->max_cll * s);
        eglSurfaceAttrib(f->dpy, f->surface, EGL_CTA861_3_MAX_FRAME_AVERAGE_LEVEL_EXT, hdr->max_fall * s);
    }
}

struct native_window {
    struct {
        int magic, version;
        void *reserved[4];
        void (*inc_ref)(void *);
        void (*dec_ref)(void *);
    } common;
    uint32_t flags;
    int min_swap, max_swap;
    float xdpi, ydpi;
    intptr_t oem[4];
    void *fns[5];
    int (*perform)(struct native_window *, int, ...);
};

static void send_hdr10p(struct android_fel *f, float peak, float avg,
                        float max_luma, const float *pct)
{
    size_t dsize;
    AVDynamicHDRPlus *d = mp_dovi_hdr10p(peak, avg, max_luma, pct, &dsize);
    if (!d)
        return;
    uint8_t *payload = NULL;
    size_t size = 0;
    int ret = av_dynamic_hdr_plus_to_t35(d, &payload, &size);
    av_free(d);
    if (ret < 0)
        return;
    uint8_t buf[512] = {0xB5, 0x00, 0x3C, 0x00, 0x01, 0x04};
    if (size + 6 <= sizeof(buf)) {
        memcpy(buf + 6, payload, size);
        struct native_window *win = (void *)vo_android_native_window(f->vo);
        int r = win->perform(win, 34, size + 6, buf);
        if (pct) {
            MP_VERBOSE(f, "HDR10+ measured peak %.0f avg %.0f p50 %.0f p99 %.0f nits -> %d\n",
                   peak, avg, pct[4], pct[8], r);
        } else {
            MP_VERBOSE(f, "HDR10+ scene peak %.0f avg %.0f nits -> %d\n", peak, avg, r);
        }
    }
    av_free(payload);
}

static void collect_stats(struct android_fel *f)
{
    int i = f->stat_idx;
    if (!f->stat_pending[i])
        return;
    f->stat_pending[i] = false;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, f->stat_pbo[i]);
    const uint32_t *px = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, STAT_W * STAT_H * 4,
                                          GL_MAP_READ_BIT);
    if (px && f->stat_scene[i] == f->scene) {
        for (int n = 0; n < STAT_W * STAT_H; n++) {
            uint32_t v = px[n];
            uint32_t m = MPMAX(MPMAX(v & 0x3ff, (v >> 10) & 0x3ff), (v >> 20) & 0x3ff);
            f->hist[m]++;
        }
        f->hist_n += STAT_W * STAT_H;
    }
    if (px)
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

static void measure(struct android_fel *f)
{
    if (!f->stat_pbo[0])
        return;
    collect_stats(f);
    int i = f->stat_idx;
    glBindFramebuffer(GL_FRAMEBUFFER, f->stat_fbo);
    glViewport(0, 0, STAT_W, STAT_H);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, f->stat_pbo[i]);
    glReadPixels(0, 0, STAT_W, STAT_H, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    f->stat_pending[i] = true;
    f->stat_scene[i] = f->scene;
    f->stat_idx = !i;
}

static void update_hdr10p(struct android_fel *f, const struct pl_hdr_metadata *hdr)
{
    if (hdr->max_pq_y <= 0)
        return;
    float peak = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, hdr->max_pq_y);
    float avg = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, hdr->avg_pq_y);
    if (peak != f->hdr10p_peak || avg != f->hdr10p_avg) {
        f->hdr10p_peak = peak;
        f->hdr10p_avg = avg;
        f->scene++;
        memset(f->hist, 0, sizeof(f->hist));
        f->hist_n = 0;
        f->since_sent = -1;
        send_hdr10p(f, peak, avg, hdr->max_luma, NULL);
        return;
    }
    if (!f->hist_n || (f->since_sent >= 0 && ++f->since_sent < 12))
        return;
    f->since_sent = 0;

    static const int percentage[9] = {1, 5, 10, 25, 50, 75, 90, 95, 99};
    float pct[9];
    double sum = 0;
    uint64_t acc = 0;
    int k = 0;
    for (int b = 0; b < STAT_BINS; b++) {
        sum += f->nits[b] * f->hist[b];
        acc += f->hist[b];
        while (k < 9 && acc * 100 >= f->hist_n * percentage[k])
            pct[k++] = f->nits[b];
    }
    send_hdr10p(f, peak, sum / f->hist_n, hdr->max_luma, pct);
}

static void release(AVMediaCodecBuffer *buf, int render)
{
    if (buf)
        av_mediacodec_release_buffer(buf, render);
}

void android_fel_render(struct android_fel *f, struct mp_image *img,
                        int64_t present_ns, bool drop)
{
    AVMediaCodecBuffer *bb = (AVMediaCodecBuffer *)img->planes[3];
    struct mp_image *eli = img->enhancement_layer;
    AVMediaCodecBuffer *eb = eli && eli->imgfmt == IMGFMT_MEDIACODEC
                             ? (AVMediaCodecBuffer *)eli->planes[3] : NULL;
    if (drop || !android_fel_active(f) || !av_mediacodec_buffer_is_current(bb)) {
        release(bb, 0);
        release(eb, 0);
        return;
    }

    const struct pl_dovi_metadata *dovi = img->params.repr.dovi;
    bool use_el = dovi && dovi->nlq_active && (eb || f->el.hist[0]);
    if (!use_el)
        release(eb, 0);
    bool new_el = use_el && eb;

    mp_mutex_lock(&f->bl.lock);
    f->bl.available = false;
    mp_mutex_unlock(&f->bl.lock);
    mp_mutex_lock(&f->el.lock);
    f->el.available = false;
    mp_mutex_unlock(&f->el.lock);

    release(bb, 1);
    if (new_el)
        release(eb, 1);
    if (!layer_acquire(f, &f->bl))
        return;
    if (new_el && !layer_acquire(f, &f->el))
        use_el = false;

    int opt = f->vo->opts->android_dovi_tonemap;
    bool tonemap = dovi && (opt < 0 ? !f->display_hdr10p : opt);
    if (tonemap) {
        struct pl_hdr_metadata hdr = img->params.color.hdr;
        hdr.max_luma = hdr.max_cll = f->display_peak;
        hdr.max_fall = MPMIN(hdr.max_fall > 0 ? hdr.max_fall : f->display_peak, f->display_peak);
        hdr.min_luma = MPMAX(hdr.min_luma, 0.005f);
        update_hdr(f, &hdr);
    } else {
        update_hdr(f, &img->params.color.hdr);
        update_hdr10p(f, &img->params.color.hdr);
    }

    EGLint sw = 0, sh = 0;
    eglQuerySurface(f->dpy, f->surface, EGL_WIDTH, &sw);
    eglQuerySurface(f->dpy, f->surface, EGL_HEIGHT, &sh);
    int dw, dh;
    mp_image_params_get_dsize(&img->params, &dw, &dh);
    int vw = sw, vh = sh;
    if (dw > 0 && dh > 0) {
        if ((int64_t)sw * dh > (int64_t)sh * dw) {
            vw = lrint(sh * (double)dw / dh);
        } else {
            vh = lrint(sw * (double)dh / dw);
        }
    }
    if (vw != sw || vh != sh) {
        glViewport(0, 0, sw, sh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glViewport((sw - vw) / 2, (sh - vh) / 2, vw, vh);

    struct program *p = &f->prog[!!dovi][use_el];
    glUseProgram(p->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, f->bl.tex);
    glUniform4fv(p->blc, 1, f->bl.crop);
    if (dovi) {
        upload_dovi(f, p, dovi);
        upload_tonemap(f, p, img);
    } else {
        upload_bt2020(p);
    }
    if (use_el) {
        float elc[4];
        memcpy(elc, f->el.crop, sizeof(elc));
        if (f->el.w < f->bl.w)
            elc[0] += 0.5f / f->bl.w * elc[2];
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, f->el.tex);
        glUniform4fv(p->elc, 1, elc);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    measure(f);

    if (present_ns > 0)
        f->PresentationTime(f->dpy, f->surface, present_ns);
    if (!eglSwapBuffers(f->dpy, f->surface))
        MP_WARN(f, "eglSwapBuffers failed: 0x%x\n", eglGetError());
}
