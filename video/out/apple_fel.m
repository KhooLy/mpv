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
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libplacebo/colorspace.h>

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <simd/simd.h>
#include <Block.h>

#ifdef APPLE_FEL_TEST
#include <stdio.h>
#define MP_ERR(obj, ...) fprintf(stderr, __VA_ARGS__)
#define MP_WARN(obj, ...) fprintf(stderr, __VA_ARGS__)
#define MP_INFO(obj, ...) ((void)0)
#define MP_VERBOSE(obj, ...) ((void)0)
#else
#include "common/msg.h"
#endif

#include "apple_fel.h"

#if PL_API_VER >= 367

#define MAX_IN_FLIGHT 3
#define MAX_MMR 48

struct fel_params {
    simd_float4 pivots[6];
    simd_float4 coeffs[24];
    simd_float4 mmr[MAX_MMR];
    simd_float4 lo, hi, act;
    simd_float4 ycc[3];
    simd_float4 ycc_off;
    simd_float4 lms[3];
    simd_float4 nlq_off, nlq_slope, nlq_thr;
    simd_float4 shift;
    float norm;
    int has_el;
    int pad[2];
};

struct apple_fel {
    struct mp_log *log;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    CVMetalTextureCacheRef cache;
    id<MTLRenderPipelineState> pipeline;
    id<MTLRenderPipelineState> pipeline_y;
    id<MTLRenderPipelineState> pipeline_c;
    bool dv;
    CVPixelBufferPoolRef pool;
    int pool_dims[2];
    atomic_int in_flight;
    atomic_int frames;
    atomic_int with_el;
};

static const char source[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "constexpr sampler smp(coord::normalized, address::clamp_to_edge, filter::linear);\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "struct Params {\n"
    "    float4 pivots[6];\n"
    "    float4 coeffs[24];\n"
    "    float4 mmr[48];\n"
    "    float4 lo, hi, act;\n"
    "    float4 ycc[3];\n"
    "    float4 ycc_off;\n"
    "    float4 lms[3];\n"
    "    float4 nlq_off, nlq_slope, nlq_thr;\n"
    "    float4 shift;\n"
    "    float norm;\n"
    "    int has_el;\n"
    "    int2 pad;\n"
    "};\n"
    "constant float M1 = 0.1593017578125, M2 = 78.84375;\n"
    "constant float C1 = 0.8359375, C2 = 18.8515625, C3 = 18.6875;\n"
    "vertex VOut vs(uint vid [[vertex_id]]) {\n"
    "    float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "    VOut o;\n"
    "    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "    o.uv = float2(p.x, 1.0 - p.y);\n"
    "    return o;\n"
    "}\n"
    "float reshape(constant Params &P, int c, float3 sig) {\n"
    "    float s = sig[c];\n"
    "    float4 a = float4(s >= P.pivots[2 * c]);\n"
    "    float4 b = float4(s >= P.pivots[2 * c + 1]);\n"
    "    int i = int(dot(a, float4(1.0)) + dot(b, float4(1.0)));\n"
    "    float4 k = P.coeffs[8 * c + i];\n"
    "    if (k.w == 0.0)\n"
    "        return (k.z * s + k.y) * s + k.x;\n"
    "    int m = int(k.y);\n"
    "    float4 x = float4(sig.xxy * sig.yzz, sig.x * sig.y * sig.z);\n"
    "    float r = k.x + dot(P.mmr[m].xyz, sig) + dot(P.mmr[m + 1], x);\n"
    "    if (k.w >= 2.0) {\n"
    "        float3 s2 = sig * sig;\n"
    "        float4 x2 = x * x;\n"
    "        r += dot(P.mmr[m + 2].xyz, s2) + dot(P.mmr[m + 3], x2);\n"
    "        if (k.w >= 3.0)\n"
    "            r += dot(P.mmr[m + 4].xyz, s2 * sig) + dot(P.mmr[m + 5], x2 * x);\n"
    "    }\n"
    "    return r;\n"
    "}\n"
    "float3 fetch(texture2d<float> ty, texture2d<float> tc, float2 uv, float norm) {\n"
    "    float2 cuv = uv + float2(0.25 / float(tc.get_width()), 0.0);\n"
    "    return float3(ty.sample(smp, uv).r, tc.sample(smp, cuv).rg) * norm;\n"
    "}\n"
    "float3 stage(float2 uv, texture2d<float> bly, texture2d<float> blc,\n"
    "             texture2d<float> ely, texture2d<float> elc, constant Params &P) {\n"
    "    float3 c = fetch(bly, blc, uv, P.norm);\n"
    "    float3 sig = clamp(c, float3(0.0), float3(1.0));\n"
    "    float3 r = float3(reshape(P, 0, sig), reshape(P, 1, sig), reshape(P, 2, sig));\n"
    "    c = mix(c, clamp(r, P.lo.xyz, P.hi.xyz), P.act.xyz);\n"
    "    if (P.has_el != 0) {\n"
    "        float2 euv = uv + float2(P.shift.x, 0.0);\n"
    "        float3 e = fetch(ely, elc, euv, P.norm) - P.nlq_off.xyz;\n"
    "        c += sign(e) * (abs(e) * P.nlq_slope.xyz + P.nlq_thr.xyz);\n"
    "    }\n"
    "    return c;\n"
    "}\n"
    "fragment float4 compose(VOut in [[stage_in]],\n"
    "                        texture2d<float> bly [[texture(0)]],\n"
    "                        texture2d<float> blc [[texture(1)]],\n"
    "                        texture2d<float> ely [[texture(2)]],\n"
    "                        texture2d<float> elc [[texture(3)]],\n"
    "                        constant Params &P [[buffer(0)]]) {\n"
    "    float3 c = stage(in.uv, bly, blc, ely, elc, P);\n"
    "    float3 d = c - P.ycc_off.xyz;\n"
    "    c = float3(dot(P.ycc[0].xyz, d), dot(P.ycc[1].xyz, d), dot(P.ycc[2].xyz, d));\n"
    "    c = pow(max(c, float3(0.0)), float3(1.0 / M2));\n"
    "    c = max(c - C1, float3(0.0)) / (C2 - C3 * c);\n"
    "    c = pow(c, float3(1.0 / M1));\n"
    "    c = float3(dot(P.lms[0].xyz, c), dot(P.lms[1].xyz, c), dot(P.lms[2].xyz, c));\n"
    "    c = pow(max(c, float3(0.0)), float3(M1));\n"
    "    c = pow((C1 + C2 * c) / (1.0 + C3 * c), float3(M2));\n"
    "    return float4(c, 1.0);\n"
    "}\n"
    "fragment float4 dv_y(VOut in [[stage_in]],\n"
    "                     texture2d<float> bly [[texture(0)]],\n"
    "                     texture2d<float> blc [[texture(1)]],\n"
    "                     texture2d<float> ely [[texture(2)]],\n"
    "                     texture2d<float> elc [[texture(3)]],\n"
    "                     constant Params &P [[buffer(0)]]) {\n"
    "    float3 c = stage(in.uv, bly, blc, ely, elc, P) / P.norm;\n"
    "    return float4(clamp(c.x, 0.0, 1.0), 0.0, 0.0, 1.0);\n"
    "}\n"
    "fragment float4 dv_c(VOut in [[stage_in]],\n"
    "                     texture2d<float> bly [[texture(0)]],\n"
    "                     texture2d<float> blc [[texture(1)]],\n"
    "                     texture2d<float> ely [[texture(2)]],\n"
    "                     texture2d<float> elc [[texture(3)]],\n"
    "                     constant Params &P [[buffer(0)]]) {\n"
    "    float2 uv = in.uv - float2(0.25 / float(blc.get_width()), 0.0);\n"
    "    float3 c = stage(uv, bly, blc, ely, elc, P) / P.norm;\n"
    "    return float4(clamp(c.yz, float2(0.0), float2(1.0)), 0.0, 1.0);\n"
    "}\n";

static void put_row(simd_float4 *dst, const float m[3][3], int row)
{
    *dst = (simd_float4){m[row][0], m[row][1], m[row][2], 0};
}

static void mat_mul(float out[3][3], const float a[3][3], const float b[3][3])
{
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    }
}

static void fill_params(struct apple_fel *f, struct fel_params *P,
                        const struct pl_dovi_metadata *d, bool has_el)
{
    memset(P, 0, sizeof(*P));
    int midx = 0;
    for (int c = 0; c < 3; c++) {
        const struct pl_reshape_data *comp = &d->comp[c];
        for (int i = 0; i < 8; i++)
            P->pivots[2 * c + i / 4][i % 4] = 1e9f;
        bool active = comp->num_pivots >= 2;
        P->act[c] = active;
        P->lo[c] = 0;
        P->hi[c] = 1;
        if (!active)
            continue;
        for (int i = 0; i < comp->num_pivots - 2; i++)
            P->pivots[2 * c + i / 4][i % 4] = comp->pivots[i + 1];
        P->lo[c] = comp->pivots[0];
        P->hi[c] = comp->pivots[comp->num_pivots - 1];
        for (int i = 0; i < comp->num_pivots - 1; i++) {
            simd_float4 *k = &P->coeffs[8 * c + i];
            if (comp->method[i] == 0) {
                *k = (simd_float4){comp->poly_coeffs[i][0], comp->poly_coeffs[i][1],
                                   comp->poly_coeffs[i][2], 0};
                continue;
            }
            int order = comp->mmr_order[i];
            if (midx + 2 * order > MAX_MMR) {
                MP_WARN(f, "Too many MMR coefficients\n");
                break;
            }
            *k = (simd_float4){comp->mmr_constant[i], midx, 0, order};
            for (int j = 0; j < order; j++) {
                const float *w = comp->mmr_coeffs[i][j];
                P->mmr[midx] = (simd_float4){w[0], w[1], w[2], 0};
                P->mmr[midx + 1] = (simd_float4){w[3], w[4], w[5], w[6]};
                midx += 2;
            }
        }
    }

    static const float lms2rgb[3][3] = {
        { 3.06441879, -2.16597676,  0.10155818},
        {-0.65612108,  1.78554118, -0.12943749},
        { 0.01736321, -0.04725154,  1.03004253},
    };
    float lms[3][3];
    mat_mul(lms, lms2rgb, d->linear.m);
    for (int i = 0; i < 3; i++) {
        put_row(&P->lms[i], lms, i);
        put_row(&P->ycc[i], d->nonlinear.m, i);
        P->ycc_off[i] = d->nonlinear_offset[i] * (1024.0f / 1023.0f);
    }

    P->has_el = has_el;
    if (has_el) {
        for (int c = 0; c < 3; c++) {
            P->nlq_off[c] = d->nlq[c].offset;
            P->nlq_slope[c] = d->nlq[c].deadzone_slope;
            P->nlq_thr[c] = d->nlq[c].deadzone_threshold;
        }
    }
    P->norm = 65535.0f / 65472.0f;
}

static bool ten_bit_420(CVPixelBufferRef pix)
{
    OSType fmt = CVPixelBufferGetPixelFormatType(pix);
    return fmt == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
           fmt == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
}

static CVMetalTextureRef plane_texture(struct apple_fel *f, CVPixelBufferRef pix, int plane)
{
    CVMetalTextureRef tex = NULL;
    CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, f->cache, pix, NULL,
        plane ? MTLPixelFormatRG16Unorm : MTLPixelFormatR16Unorm,
        CVPixelBufferGetWidthOfPlane(pix, plane),
        CVPixelBufferGetHeightOfPlane(pix, plane), plane, &tex);
    return tex;
}

static CVPixelBufferPoolRef create_pool(OSType format, int w, int h)
{
    NSDictionary *attrs = @{
        (id)kCVPixelBufferPixelFormatTypeKey: @(format),
        (id)kCVPixelBufferWidthKey: @(w),
        (id)kCVPixelBufferHeightKey: @(h),
        (id)kCVPixelBufferMetalCompatibilityKey: @YES,
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    CVPixelBufferPoolRef pool = NULL;
    CVPixelBufferPoolCreate(kCFAllocatorDefault, NULL, (CFDictionaryRef)attrs, &pool);
    return pool;
}

static CVPixelBufferRef create_output(struct apple_fel *f, int w, int h)
{
    int dims[2] = {w, h};
    if (!f->pool || memcmp(f->pool_dims, dims, sizeof(dims))) {
        if (f->pool)
            CVPixelBufferPoolRelease(f->pool);
        if (f->dv) {
            f->pool = create_pool(kCVPixelFormatType_422YpCbCr16BiPlanarVideoRange, w, h);
            if (!f->pool)
                f->pool = create_pool(kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange, w, h);
        } else {
            f->pool = create_pool(kCVPixelFormatType_ARGB2101010LEPacked, w, h);
        }
        memcpy(f->pool_dims, dims, sizeof(dims));
    }
    CVPixelBufferRef out = NULL;
    if (!f->pool || CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, f->pool,
                                                        &out) != kCVReturnSuccess)
        return NULL;
    return out;
}

static void copy_attachment(CVPixelBufferRef from, CVPixelBufferRef to, CFStringRef key)
{
    CFTypeRef value = CVBufferCopyAttachment(from, key, NULL);
    if (value) {
        CVBufferSetAttachment(to, key, value, kCVAttachmentMode_ShouldPropagate);
        CFRelease(value);
    }
}

struct apple_fel *apple_fel_create(struct mp_log *log, bool dv_domain)
{
    struct apple_fel *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->log = log;
    f->dv = dv_domain;
    f->device = MTLCreateSystemDefaultDevice();
    if (!f->device) {
        MP_VERBOSE(f, "No Metal device for Dolby Vision composition\n");
        goto fail;
    }
    f->queue = [f->device newCommandQueue];
    if (CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, f->device, NULL,
                                  &f->cache) != kCVReturnSuccess)
        goto fail;

    NSError *error = nil;
    id<MTLLibrary> lib = [f->device newLibraryWithSource:[NSString stringWithUTF8String:source]
                                                 options:nil error:&error];
    if (!lib) {
        MP_ERR(f, "Dolby Vision Metal compile failed: %s\n",
               error.localizedDescription.UTF8String);
        goto fail;
    }
    MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = [[lib newFunctionWithName:@"vs"] autorelease];
    if (f->dv) {
        desc.fragmentFunction = [[lib newFunctionWithName:@"dv_y"] autorelease];
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatR16Unorm;
        f->pipeline_y = [f->device newRenderPipelineStateWithDescriptor:desc error:&error];
        desc.fragmentFunction = [[lib newFunctionWithName:@"dv_c"] autorelease];
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatRG16Unorm;
        f->pipeline_c = [f->device newRenderPipelineStateWithDescriptor:desc error:&error];
    } else {
        desc.fragmentFunction = [[lib newFunctionWithName:@"compose"] autorelease];
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGR10A2Unorm;
        f->pipeline = [f->device newRenderPipelineStateWithDescriptor:desc error:&error];
    }
    [desc release];
    [lib release];
    if (f->dv ? !(f->pipeline_y && f->pipeline_c) : !f->pipeline) {
        MP_ERR(f, "Dolby Vision pipeline failed: %s\n", error.localizedDescription.UTF8String);
        goto fail;
    }
    MP_VERBOSE(f, "Dolby Vision composition ready\n");
    return f;

fail:
    apple_fel_destroy(f);
    return NULL;
}

int apple_fel_compose(struct apple_fel *f, void *bl_buf, void *el_buf,
                      const struct pl_dovi_metadata *dovi, void *done_block)
{
    CVPixelBufferRef bl = bl_buf, el = el_buf;
    void (^done)(CVPixelBufferRef) = done_block;
    if (!dovi || !ten_bit_420(bl) || (el && !ten_bit_420(el)))
        return APPLE_FEL_UNSUPPORTED;
    if (atomic_load(&f->in_flight) >= MAX_IN_FLIGHT)
        return APPLE_FEL_BUSY;

    @autoreleasepool {
        int w = (int)CVPixelBufferGetWidth(bl);
        int h = (int)CVPixelBufferGetHeight(bl);
        CVPixelBufferRef out = create_output(f, w, h);
        CVMetalTextureRef bl_y = plane_texture(f, bl, 0);
        CVMetalTextureRef bl_c = plane_texture(f, bl, 1);
        CVMetalTextureRef el_y = el ? plane_texture(f, el, 0) : NULL;
        CVMetalTextureRef el_c = el ? plane_texture(f, el, 1) : NULL;
        CVMetalTextureRef target = NULL;
        CVMetalTextureRef target_c = NULL;
        if (out && f->dv) {
            CVMetalTextureCacheCreateTextureFromImage(
                kCFAllocatorDefault, f->cache, out, NULL, MTLPixelFormatR16Unorm,
                w, h, 0, &target);
            CVMetalTextureCacheCreateTextureFromImage(
                kCFAllocatorDefault, f->cache, out, NULL, MTLPixelFormatRG16Unorm,
                w / 2, h, 1, &target_c);
        } else if (out) {
            CVMetalTextureCacheCreateTextureFromImage(
                kCFAllocatorDefault, f->cache, out, NULL, MTLPixelFormatBGR10A2Unorm,
                w, h, 0, &target);
        }

        bool use_el = el && el_y && el_c && dovi->nlq_active;
        bool ok = out && bl_y && bl_c && target && (!f->dv || target_c) &&
                  !(el && dovi->nlq_active && !use_el);
        if (ok) {
            struct fel_params P;
            fill_params(f, &P, dovi, use_el);
            P.shift[0] = 0.5f / w;

            id<MTLCommandBuffer> cb = [f->queue commandBuffer];
            id<MTLTexture> blt = CVMetalTextureGetTexture(bl_y);
            id<MTLTexture> blc = CVMetalTextureGetTexture(bl_c);
            id<MTLTexture> elt = use_el ? CVMetalTextureGetTexture(el_y) : blt;
            id<MTLTexture> elc = use_el ? CVMetalTextureGetTexture(el_c) : blc;
            int passes = f->dv ? 2 : 1;
            for (int i = 0; i < passes; i++) {
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture =
                    CVMetalTextureGetTexture(i ? target_c : target);
                rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                [enc setRenderPipelineState:!f->dv ? f->pipeline : i ? f->pipeline_c : f->pipeline_y];
                [enc setFragmentTexture:blt atIndex:0];
                [enc setFragmentTexture:blc atIndex:1];
                [enc setFragmentTexture:elt atIndex:2];
                [enc setFragmentTexture:elc atIndex:3];
                [enc setFragmentBytes:&P length:sizeof(P) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding];
            }

            copy_attachment(bl, out, kCVImageBufferMasteringDisplayColorVolumeKey);
            copy_attachment(bl, out, kCVImageBufferContentLightLevelInfoKey);
            if (!f->dv) {
                CVBufferSetAttachment(out, kCVImageBufferColorPrimariesKey,
                                      kCVImageBufferColorPrimaries_ITU_R_2020,
                                      kCVAttachmentMode_ShouldPropagate);
                CVBufferSetAttachment(out, kCVImageBufferTransferFunctionKey,
                                      kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ,
                                      kCVAttachmentMode_ShouldPropagate);
            }

            CFRetain(bl);
            if (el)
                CFRetain(el);
            atomic_fetch_add(&f->in_flight, 1);
            void (^callback)(CVPixelBufferRef) = Block_copy(done);
            [cb addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
                if (buffer.status == MTLCommandBufferStatusCompleted) {
                    int n = atomic_fetch_add(&f->frames, 1);
                    int e = use_el ? atomic_fetch_add(&f->with_el, 1) + 1
                                   : atomic_load(&f->with_el);
                    if (n == 23)
                        MP_INFO(f, "fel: frames=%d with_el=%d\n", n + 1, e);
                    callback(out);
                } else {
                    MP_ERR(f, "Metal command buffer failed: %s\n",
                           buffer.error.localizedDescription.UTF8String);
                }
                Block_release(callback);
                CFRelease(bl_y);
                CFRelease(bl_c);
                if (el_y)
                    CFRelease(el_y);
                if (el_c)
                    CFRelease(el_c);
                CFRelease(target);
                if (target_c)
                    CFRelease(target_c);
                CVPixelBufferRelease(out);
                CFRelease(bl);
                if (el)
                    CFRelease(el);
                CVMetalTextureCacheFlush(f->cache, 0);
                atomic_fetch_sub(&f->in_flight, 1);
            }];
            [cb commit];
            return APPLE_FEL_QUEUED;
        }

        MP_ERR(f, "Could not create Metal textures for Dolby Vision composition\n");
        if (bl_y)
            CFRelease(bl_y);
        if (bl_c)
            CFRelease(bl_c);
        if (el_y)
            CFRelease(el_y);
        if (el_c)
            CFRelease(el_c);
        if (target)
            CFRelease(target);
        if (target_c)
            CFRelease(target_c);
        if (out)
            CVPixelBufferRelease(out);
    }
    return APPLE_FEL_UNSUPPORTED;
}

void apple_fel_destroy(struct apple_fel *f)
{
    if (!f)
        return;
    while (atomic_load(&f->in_flight) > 0)
        usleep(1000);
    if (f->pool)
        CVPixelBufferPoolRelease(f->pool);
    if (f->cache)
        CFRelease(f->cache);
    [f->pipeline release];
    [f->pipeline_y release];
    [f->pipeline_c release];
    [f->queue release];
    [f->device release];
    free(f);
}

#else

struct apple_fel *apple_fel_create(struct mp_log *log, bool dv_domain)
{
    return NULL;
}

int apple_fel_compose(struct apple_fel *f, void *bl, void *el,
                      const struct pl_dovi_metadata *dovi, void *done)
{
    return APPLE_FEL_UNSUPPORTED;
}

void apple_fel_destroy(struct apple_fel *f)
{
}

#endif
