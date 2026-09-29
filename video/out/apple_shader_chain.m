#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#include <Block.h>
#include <math.h>
#include <stdatomic.h>

#include "common/msg.h"
#include "misc/bstr.h"
#include "options/path.h"
#include "stream/stream.h"
#include "video/out/gpu/user_shaders.h"
#include "apple_shader_chain.h"
#include "apple_shaders.h"

#define MAX_IN_FLIGHT 3
#define MAX_FREE_TEXTURES 24

enum stage {
    STAGE_MAIN,
    STAGE_PREKERNEL,
    STAGE_NONE,
};

struct pass {
    struct gl_user_shader_hook hook;
    enum stage stage;
    id<MTLRenderPipelineState> pipeline;
};

struct apple_shader_chain {
    struct mp_log *log;
    void *ta;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    CVMetalTextureCacheRef cache;
    id<MTLRenderPipelineState> convert;
    id<MTLRenderPipelineState> copy;
    struct pass *passes;
    int num_passes;
    dispatch_group_t compiling;
    atomic_bool ready;
    atomic_int in_flight;
    NSMutableArray *free_textures;
    CVPixelBufferPoolRef out_pool;
    int pool_dims[4];
    atomic_int frames;
    atomic_int skipped;
    NSObject *stats_lock;
    int checked;
    double in_luma, out_luma, out_std;
};

struct convert_params {
    float y_offset, y_scale, c_offset, c_scale;
    float kr, kb, norm, pad;
};

struct run {
    struct apple_shader_chain *chain;
    NSMutableArray *held;
    NSMutableDictionary *saved;
    id<MTLTexture> main;
    id<MTLTexture> native;
    int out_w, out_h;
};

static const char builtin_source[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "constexpr sampler smp(coord::normalized, address::clamp_to_edge, filter::linear);\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "struct Conv { float y_offset, y_scale, c_offset, c_scale, kr, kb, norm, pad; };\n"
    "vertex VOut vs(uint vid [[vertex_id]]) {\n"
    "    float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "    VOut o;\n"
    "    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "    o.uv = float2(p.x, 1.0 - p.y);\n"
    "    return o;\n"
    "}\n"
    "fragment float4 convert(VOut in [[stage_in]],\n"
    "                        texture2d<float> ty [[texture(0)]],\n"
    "                        texture2d<float> tc [[texture(1)]],\n"
    "                        constant Conv &c [[buffer(0)]]) {\n"
    "    float y = (ty.sample(smp, in.uv).r * c.norm - c.y_offset) * c.y_scale;\n"
    "    float2 cc = (tc.sample(smp, in.uv).rg * c.norm - c.c_offset) * c.c_scale;\n"
    "    float r = y + 2.0 * (1.0 - c.kr) * cc.y;\n"
    "    float b = y + 2.0 * (1.0 - c.kb) * cc.x;\n"
    "    float g = (y - c.kr * r - c.kb * b) / (1.0 - c.kr - c.kb);\n"
    "    return float4(r, g, b, 1.0);\n"
    "}\n"
    "fragment float4 copy(VOut in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
    "    return float4(saturate(t.sample(smp, in.uv).rgb), 1.0);\n"
    "}\n";

static NSString *ns_string(struct bstr s)
{
    return [[[NSString alloc] initWithBytes:s.start
                                     length:s.len
                                   encoding:NSUTF8StringEncoding] autorelease];
}

static int count_names(const struct bstr *names, int max)
{
    int n = 0;
    while (n < max && names[n].start && names[n].len)
        n++;
    return n;
}

static enum stage pass_stage(const struct gl_user_shader_hook *hook)
{
    int n = count_names(hook->hook_tex, SHADER_MAX_HOOKS);
    for (int i = 0; i < n; i++) {
        if (bstr_equals0(hook->hook_tex[i], "MAIN") ||
            bstr_equals0(hook->hook_tex[i], "RGB"))
            return STAGE_MAIN;
    }
    for (int i = 0; i < n; i++) {
        if (bstr_equals0(hook->hook_tex[i], "PREKERNEL"))
            return STAGE_PREKERNEL;
    }
    return STAGE_NONE;
}

static bool add_hook(void *priv, const char *path,
                     const struct gl_user_shader_hook *hook)
{
    struct apple_shader_chain *c = priv;
    struct pass pass = {.hook = *hook, .stage = pass_stage(hook)};
    if (pass.stage == STAGE_NONE) {
        MP_WARN(c->log, "Skipping pass '%.*s': only MAIN and PREKERNEL hooks are supported\n",
                BSTR_P(hook->pass_desc));
        return true;
    }
    MP_TARRAY_APPEND(c->ta, c->passes, c->num_passes, pass);
    return true;
}

static bool add_tex(void *priv, struct gl_user_shader_tex tex)
{
    return false;
}

static bool compile_pass(struct apple_shader_chain *c, struct pass *pass)
{
    const struct gl_user_shader_hook *h = &pass->hook;
    int nb = count_names(h->bind_tex, SHADER_MAX_BINDS);
    const char *names[SHADER_MAX_BINDS];
    size_t lens[SHADER_MAX_BINDS];
    for (int i = 0; i < nb; i++) {
        names[i] = (const char *)h->bind_tex[i].start;
        lens[i] = h->bind_tex[i].len;
    }
    struct bstr hooked = h->hook_tex[0];
    if (pass->stage == STAGE_PREKERNEL)
        hooked = bstr0("PREKERNEL");
    else
        hooked = bstr0("MAIN");
    struct apple_pass_source src = {
        .body = (const char *)h->pass_body.start,
        .body_len = h->pass_body.len,
        .binds = names,
        .bind_lens = lens,
        .num_binds = nb,
        .hooked = (const char *)hooked.start,
        .hooked_len = hooked.len,
    };
    char *msl = apple_shader_msl(&src);
    if (!msl)
        return false;

    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:msl];
    free(msl);
    id<MTLLibrary> lib = [c->device newLibraryWithSource:source options:nil error:&error];
    if (!lib) {
        MP_ERR(c->log, "Metal compile failed for '%.*s': %s\n", BSTR_P(h->pass_desc),
               error.localizedDescription.UTF8String);
        return false;
    }
    MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = [[lib newFunctionWithName:@"vs"] autorelease];
    desc.fragmentFunction = [[lib newFunctionWithName:@"fs"] autorelease];
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    pass->pipeline = [c->device newRenderPipelineStateWithDescriptor:desc error:&error];
    [desc release];
    [lib release];
    if (!pass->pipeline)
        MP_ERR(c->log, "Pipeline failed for '%.*s': %s\n", BSTR_P(h->pass_desc),
               error.localizedDescription.UTF8String);
    return pass->pipeline != nil;
}

static id<MTLRenderPipelineState> builtin_pipeline(struct apple_shader_chain *c,
                                                   id<MTLLibrary> lib, NSString *fragment,
                                                   MTLPixelFormat format)
{
    NSError *error = nil;
    MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = [[lib newFunctionWithName:@"vs"] autorelease];
    desc.fragmentFunction = [[lib newFunctionWithName:fragment] autorelease];
    desc.colorAttachments[0].pixelFormat = format;
    id<MTLRenderPipelineState> state =
        [c->device newRenderPipelineStateWithDescriptor:desc error:&error];
    [desc release];
    if (!state)
        MP_ERR(c->log, "Pipeline '%s' failed: %s\n", fragment.UTF8String,
               error.localizedDescription.UTF8String);
    return state;
}

struct apple_shader_chain *apple_shader_chain_create(struct mpv_global *global,
                                                     struct mp_log *log, char **paths)
{
    if (!paths || !paths[0])
        return NULL;

    struct apple_shader_chain *c = talloc_zero(NULL, struct apple_shader_chain);
    c->log = log;
    c->ta = c;
    c->device = MTLCreateSystemDefaultDevice();
    if (!c->device) {
        MP_ERR(log, "No Metal device\n");
        goto fail;
    }
    c->queue = [c->device newCommandQueue];
    if (CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, c->device, NULL,
                                  &c->cache) != kCVReturnSuccess)
        goto fail;
    c->free_textures = [[NSMutableArray alloc] init];
    c->stats_lock = [[NSObject alloc] init];

    for (int n = 0; paths[n]; n++) {
        char *fname = mp_get_user_path(NULL, global, paths[n]);
        struct bstr body = stream_read_file(fname, c, global, 100000000);
        talloc_free(fname);
        if (!body.len) {
            MP_ERR(log, "Could not read shader %s\n", paths[n]);
            continue;
        }
        parse_user_shader(log, NULL, body, paths[n], c, add_hook, add_tex);
    }
    if (!c->num_passes)
        goto fail;

    c->compiling = dispatch_group_create();
    dispatch_group_async(c->compiling,
                         dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        NSString *source = [NSString stringWithUTF8String:builtin_source];
        id<MTLLibrary> lib = [c->device newLibraryWithSource:source options:nil error:&error];
        if (!lib) {
            MP_ERR(c->log, "Builtin Metal source failed: %s\n",
                   error.localizedDescription.UTF8String);
            return;
        }
        c->convert = builtin_pipeline(c, lib, @"convert", MTLPixelFormatRGBA16Float);
        c->copy = builtin_pipeline(c, lib, @"copy", MTLPixelFormatBGRA8Unorm);
        [lib release];
        int compiled = 0;
        for (int i = 0; i < c->num_passes; i++)
            compiled += compile_pass(c, &c->passes[i]);
        MP_VERBOSE(c->log, "Compiled %d of %d shader passes\n", compiled, c->num_passes);
        atomic_store(&c->ready, c->convert && c->copy && compiled == c->num_passes);
    });
    return c;

fail:
    apple_shader_chain_destroy(c);
    return NULL;
}

bool apple_shader_chain_ready(struct apple_shader_chain *c)
{
    return atomic_load(&c->ready);
}

static id<MTLTexture> acquire(struct run *r, int w, int h)
{
    struct apple_shader_chain *c = r->chain;
    id<MTLTexture> tex = nil;
    for (NSUInteger i = 0; i < c->free_textures.count; i++) {
        id<MTLTexture> t = c->free_textures[i];
        if (t.width == w && t.height == h) {
            tex = [t retain];
            [c->free_textures removeObjectAtIndex:i];
            break;
        }
    }
    if (!tex) {
        MTLTextureDescriptor *desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                               width:w
                                                              height:h
                                                           mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        tex = [c->device newTextureWithDescriptor:desc];
    }
    [r->held addObject:tex];
    [tex release];
    return tex;
}

static void give_back(struct run *r, id<MTLTexture> tex)
{
    if (!tex || ![r->held containsObject:tex])
        return;
    [r->chain->free_textures addObject:tex];
    [r->held removeObject:tex];
}

static id<MTLTexture> find_tex(struct run *r, struct bstr name)
{
    if (bstr_equals0(name, "HOOKED") || bstr_equals0(name, "MAIN") ||
        bstr_equals0(name, "PREKERNEL") || bstr_equals0(name, "RGB"))
        return r->main;
    if (bstr_equals0(name, "NATIVE"))
        return r->native;
    return r->saved[ns_string(name)];
}

static bool lookup_tex(void *priv, struct bstr var, float size[2])
{
    struct run *r = priv;
    if (bstr_equals0(var, "OUTPUT")) {
        size[0] = r->out_w;
        size[1] = r->out_h;
        return true;
    }
    id<MTLTexture> tex = find_tex(r, var);
    if (!tex)
        return false;
    size[0] = tex.width;
    size[1] = tex.height;
    return true;
}

static bool lookup_param(void *priv, struct bstr var, float *out)
{
    return false;
}

static int eval_size(struct run *r, struct szexp expr[MAX_SZEXP_SIZE])
{
    float v = 0;
    if (!eval_szexpr(r->chain->log, r, lookup_tex, lookup_param, expr, &v))
        return 0;
    return MPMAX(1, lroundf(v));
}

static bool bound_later(struct apple_shader_chain *c, int stage, int index, struct bstr name)
{
    for (int i = 0; i < c->num_passes; i++) {
        struct pass *p = &c->passes[i];
        if (!p->pipeline)
            continue;
        if (p->stage < (int)stage || (p->stage == (int)stage && i <= index))
            continue;
        int nb = count_names(p->hook.bind_tex, SHADER_MAX_BINDS);
        for (int b = 0; b < nb; b++) {
            if (bstr_equals(p->hook.bind_tex[b], name))
                return true;
        }
    }
    return false;
}

static void drop_dead_textures(struct run *r, int stage, int index)
{
    for (NSString *key in [r->saved allKeys]) {
        struct bstr name = bstr0(key.UTF8String);
        if (bound_later(r->chain, stage, index, name))
            continue;
        id<MTLTexture> tex = r->saved[key];
        [r->saved removeObjectForKey:key];
        if (tex != r->main && tex != r->native)
            give_back(r, tex);
    }
}

static bool encode_pass(struct run *r, id<MTLCommandBuffer> cb, struct pass *pass,
                        int stage, int index)
{
    struct gl_user_shader_hook *h = &pass->hook;
    float cond = 0;
    if (!eval_szexpr(r->chain->log, r, lookup_tex, lookup_param, h->cond, &cond) ||
        cond <= 0)
        return true;

    int nb = count_names(h->bind_tex, SHADER_MAX_BINDS);
    id<MTLTexture> inputs[SHADER_MAX_BINDS];
    float sizes[SHADER_MAX_BINDS][2];
    for (int i = 0; i < nb; i++) {
        inputs[i] = find_tex(r, h->bind_tex[i]);
        if (!inputs[i])
            return false;
        sizes[i][0] = inputs[i].width;
        sizes[i][1] = inputs[i].height;
    }

    int w = eval_size(r, h->width);
    int hgt = eval_size(r, h->height);
    if (!w || !hgt)
        return false;

    bool to_main = !h->save_tex.len || bstr_equals0(h->save_tex, "MAIN") ||
                   bstr_equals0(h->save_tex, "PREKERNEL") ||
                   bstr_equals0(h->save_tex, "HOOKED");
    id<MTLTexture> target = acquire(r, w, hgt);

    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pass->pipeline];
    for (int i = 0; i < nb; i++)
        [enc setFragmentTexture:inputs[i] atIndex:i];
    [enc setFragmentBytes:sizes length:sizeof(sizes[0]) * nb atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];

    if (to_main) {
        id<MTLTexture> old = r->main;
        r->main = target;
        if (old != r->native && !bound_later(r->chain, stage, index, bstr0("MAIN")) &&
            !bound_later(r->chain, stage, index, bstr0("HOOKED")))
            give_back(r, old);
        else if (old != r->native)
            give_back(r, old);
    } else {
        NSString *key = ns_string(h->save_tex);
        id<MTLTexture> old = r->saved[key];
        r->saved[key] = target;
        if (old && old != r->main && old != r->native)
            give_back(r, old);
    }
    drop_dead_textures(r, stage, index);
    return true;
}

static bool convert_params(CVPixelBufferRef pix, struct convert_params *out, bool *ten_bit)
{
    OSType fmt = CVPixelBufferGetPixelFormatType(pix);
    bool full;
    switch (fmt) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        full = false;
        *ten_bit = false;
        break;
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        full = true;
        *ten_bit = false;
        break;
    case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
        full = false;
        *ten_bit = true;
        break;
    case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
        full = true;
        *ten_bit = true;
        break;
    default:
        return false;
    }

    double max = *ten_bit ? 1023.0 : 255.0;
    double shift = *ten_bit ? 4.0 : 1.0;
    *out = (struct convert_params){
        .y_offset = full ? 0 : 16 * shift / max,
        .y_scale = full ? 1 : max / (219 * shift),
        .c_offset = 128 * shift / max,
        .c_scale = full ? 1 : max / (224 * shift),
        .norm = *ten_bit ? 65535.0 / 65472.0 : 1.0,
    };

    CFTypeRef matrix = CVBufferCopyAttachment(pix, kCVImageBufferYCbCrMatrixKey, NULL);
    bool bt601 = matrix && CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_601_4);
    bool bt2020 = matrix && CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_2020);
    if (matrix)
        CFRelease(matrix);
    if (!matrix && CVPixelBufferGetHeight(pix) < 720)
        bt601 = true;
    out->kr = bt2020 ? 0.2627f : bt601 ? 0.299f : 0.2126f;
    out->kb = bt2020 ? 0.0593f : bt601 ? 0.114f : 0.0722f;
    return true;
}

static bool is_hdr(CVPixelBufferRef pix)
{
    CFTypeRef transfer = CVBufferCopyAttachment(pix, kCVImageBufferTransferFunctionKey, NULL);
    bool hdr = transfer &&
               (CFEqual(transfer, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ) ||
                CFEqual(transfer, kCVImageBufferTransferFunction_ITU_R_2100_HLG));
    if (transfer)
        CFRelease(transfer);
    return hdr;
}

static CVPixelBufferRef create_output(struct apple_shader_chain *c, int w, int h)
{
    int dims[4] = {w, h, 0, 0};
    if (!c->out_pool || memcmp(c->pool_dims, dims, sizeof(dims))) {
        if (c->out_pool)
            CVPixelBufferPoolRelease(c->out_pool);
        NSDictionary *attrs = @{
            (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferWidthKey: @(w),
            (id)kCVPixelBufferHeightKey: @(h),
            (id)kCVPixelBufferMetalCompatibilityKey: @YES,
            (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        };
        CVPixelBufferPoolCreate(kCFAllocatorDefault, NULL, (CFDictionaryRef)attrs,
                                &c->out_pool);
        memcpy(c->pool_dims, dims, sizeof(dims));
        [c->free_textures removeAllObjects];
    }
    CVPixelBufferRef out = NULL;
    if (!c->out_pool || CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, c->out_pool,
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

static void log_stats(struct apple_shader_chain *c)
{
    @synchronized (c->stats_lock) {
        double n = MPMAX(c->checked, 1);
        MP_INFO(c->log, "shader-chain: frames=%d skipped=%d checked=%d in_luma=%.4f "
                "out_luma=%.4f out_std=%.4f passes=%d\n",
                atomic_load(&c->frames), atomic_load(&c->skipped), c->checked,
                c->in_luma / n, c->out_luma / n, c->out_std / n, c->num_passes);
    }
}

static double plane_luma(CVPixelBufferRef pix, bool ten_bit)
{
    double sum = 0;
    long n = 0;
    CVPixelBufferLockBaseAddress(pix, kCVPixelBufferLock_ReadOnly);
    size_t w = CVPixelBufferGetWidthOfPlane(pix, 0);
    size_t h = CVPixelBufferGetHeightOfPlane(pix, 0);
    size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pix, 0);
    const uint8_t *base = CVPixelBufferGetBaseAddressOfPlane(pix, 0);
    for (size_t y = 0; y < h; y += 8) {
        for (size_t x = 0; x < w; x += 8) {
            double v = ten_bit ? (((const uint16_t *)(base + y * stride))[x] >> 6) / 1023.0
                               : base[y * stride + x] / 255.0;
            double lo = ten_bit ? 64 / 1023.0 : 16 / 255.0;
            double range = ten_bit ? 876 / 1023.0 : 219 / 255.0;
            sum += (v - lo) / range;
            n++;
        }
    }
    CVPixelBufferUnlockBaseAddress(pix, kCVPixelBufferLock_ReadOnly);
    return n ? sum / n : 0;
}

static void bgra_stats(CVPixelBufferRef pix, double *mean, double *std)
{
    double sum = 0, sq = 0;
    long n = 0;
    CVPixelBufferLockBaseAddress(pix, kCVPixelBufferLock_ReadOnly);
    size_t w = CVPixelBufferGetWidth(pix);
    size_t h = CVPixelBufferGetHeight(pix);
    size_t stride = CVPixelBufferGetBytesPerRow(pix);
    const uint8_t *base = CVPixelBufferGetBaseAddress(pix);
    for (size_t y = 0; y < h; y += 8) {
        for (size_t x = 0; x < w; x += 8) {
            const uint8_t *px = base + y * stride + x * 4;
            double l = (0.0722 * px[0] + 0.7152 * px[1] + 0.2126 * px[2]) / 255.0;
            sum += l;
            sq += l * l;
            n++;
        }
    }
    CVPixelBufferUnlockBaseAddress(pix, kCVPixelBufferLock_ReadOnly);
    *mean = n ? sum / n : 0;
    *std = n ? sqrt(fmax(0, sq / n - (*mean) * (*mean))) : 0;
}

static void sample_frame(struct apple_shader_chain *c, CVPixelBufferRef in,
                         CVPixelBufferRef out, bool ten_bit)
{
    double in_luma = plane_luma(in, ten_bit);
    double out_luma, out_std;
    bgra_stats(out, &out_luma, &out_std);
    @synchronized (c->stats_lock) {
        c->checked++;
        c->in_luma += in_luma;
        c->out_luma += out_luma;
        c->out_std += out_std;
    }
}

bool apple_shader_chain_run(struct apple_shader_chain *c, void *pixbuf, int out_w,
                            int out_h, void *done_block)
{
    CVPixelBufferRef in = pixbuf;
    void (^done)(CVPixelBufferRef) = done_block;
    if (!atomic_load(&c->ready) || atomic_load(&c->in_flight) >= MAX_IN_FLIGHT ||
        is_hdr(in)) {
        atomic_fetch_add(&c->skipped, 1);
        return false;
    }

    struct convert_params conv;
    bool ten_bit;
    if (!convert_params(in, &conv, &ten_bit))
        return false;

    bool ok = false;
    @autoreleasepool {
        int in_w = (int)CVPixelBufferGetWidth(in);
        int in_h = (int)CVPixelBufferGetHeight(in);
        CVMetalTextureRef luma = NULL, chroma = NULL;
        CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, c->cache, in, NULL,
            ten_bit ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm, in_w, in_h, 0, &luma);
        CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, c->cache, in, NULL,
            ten_bit ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm, in_w / 2, in_h / 2,
            1, &chroma);
        int fit_w = in_w, fit_h = in_h;
        if (out_w > 0 && out_h > 0) {
            double fit = fmin((double)out_w / in_w, (double)out_h / in_h);
            fit_w = MPMAX(2, lround(in_w * fit) & ~1);
            fit_h = MPMAX(2, lround(in_h * fit) & ~1);
        }
        CVPixelBufferRef out = create_output(c, fit_w, fit_h);
        if (!luma || !chroma || !out) {
            if (luma)
                CFRelease(luma);
            if (chroma)
                CFRelease(chroma);
            if (out)
                CVPixelBufferRelease(out);
            return false;
        }

        struct run r = {
            .chain = c,
            .held = [[NSMutableArray alloc] init],
            .saved = [[NSMutableDictionary alloc] init],
            .out_w = fit_w,
            .out_h = fit_h,
        };
        id<MTLCommandBuffer> cb = [c->queue commandBuffer];

        r.main = acquire(&r, in_w, in_h);
        r.native = r.main;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = r.main;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:c->convert];
        [enc setFragmentTexture:CVMetalTextureGetTexture(luma) atIndex:0];
        [enc setFragmentTexture:CVMetalTextureGetTexture(chroma) atIndex:1];
        [enc setFragmentBytes:&conv length:sizeof(conv) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];

        ok = true;
        for (int s = STAGE_MAIN; s <= STAGE_PREKERNEL && ok; s++) {
            for (int i = 0; i < c->num_passes && ok; i++) {
                if (c->passes[i].stage == s)
                    ok = encode_pass(&r, cb, &c->passes[i], s, i);
            }
        }

        CVMetalTextureRef target = NULL;
        if (ok)
            ok = CVMetalTextureCacheCreateTextureFromImage(
                     kCFAllocatorDefault, c->cache, out, NULL, MTLPixelFormatBGRA8Unorm,
                     CVPixelBufferGetWidth(out), CVPixelBufferGetHeight(out), 0,
                     &target) == kCVReturnSuccess;
        if (ok) {
            rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = CVMetalTextureGetTexture(target);
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            enc = [cb renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:c->copy];
            [enc setFragmentTexture:r.main atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
            copy_attachment(in, out, kCVImageBufferColorPrimariesKey);
            copy_attachment(in, out, kCVImageBufferTransferFunctionKey);
        }

        for (id<MTLTexture> t in [[r.held copy] autorelease])
            give_back(&r, t);
        while (c->free_textures.count > MAX_FREE_TEXTURES)
            [c->free_textures removeObjectAtIndex:0];
        [r.held release];
        [r.saved release];

        if (ok) {
            CFRetain(in);
            atomic_fetch_add(&c->in_flight, 1);
            void (^callback)(CVPixelBufferRef) = Block_copy(done);
            [cb addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
                if (buffer.status == MTLCommandBufferStatusCompleted) {
                    int n = atomic_fetch_add(&c->frames, 1);
                    if (n % 12 == 0)
                        sample_frame(c, in, out, ten_bit);
                    if (n % 60 == 59)
                        log_stats(c);
                    callback(out);
                } else {
                    MP_ERR(c->log, "Metal command buffer failed: %s\n",
                           buffer.error.localizedDescription.UTF8String);
                }
                Block_release(callback);
                CFRelease(luma);
                CFRelease(chroma);
                CFRelease(target);
                CVPixelBufferRelease(out);
                CFRelease(in);
                CVMetalTextureCacheFlush(c->cache, 0);
                atomic_fetch_sub(&c->in_flight, 1);
            }];
            [cb commit];
        } else {
            CFRelease(luma);
            CFRelease(chroma);
            if (target)
                CFRelease(target);
            CVPixelBufferRelease(out);
        }
    }
    return ok;
}

void apple_shader_chain_destroy(struct apple_shader_chain *c)
{
    if (!c)
        return;
    if (c->compiling) {
        dispatch_group_wait(c->compiling, DISPATCH_TIME_FOREVER);
        dispatch_release(c->compiling);
    }
    if (c->queue) {
        id<MTLCommandBuffer> cb = [c->queue commandBuffer];
        [cb commit];
        [cb waitUntilCompleted];
    }
    log_stats(c);
    [c->stats_lock release];
    for (int i = 0; i < c->num_passes; i++)
        [c->passes[i].pipeline release];
    [c->convert release];
    [c->copy release];
    [c->free_textures release];
    if (c->out_pool)
        CVPixelBufferPoolRelease(c->out_pool);
    if (c->cache)
        CFRelease(c->cache);
    [c->queue release];
    [c->device release];
    talloc_free(c);
}
