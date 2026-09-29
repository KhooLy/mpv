#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apple_shaders.h"

struct buf {
    char *data;
    size_t len, cap;
};

static void put(struct buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->data = realloc(b->data, b->cap);
    }
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, n + 1, fmt, ap);
    va_end(ap);
    b->len += n;
}

static void put_raw(struct buf *b, const char *s, size_t n)
{
    put(b, "%.*s", (int)n, s);
}

static const char prelude[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "#define vec2 float2\n"
    "#define vec3 float3\n"
    "#define vec4 float4\n"
    "#define ivec2 int2\n"
    "#define ivec3 int3\n"
    "#define ivec4 int4\n"
    "#define mat2 float2x2\n"
    "#define mat3 float3x3\n"
    "#define mat4 float4x4\n"
    "#define mod(x, y) ((x) - (y) * floor((x) / (y)))\n"
    "#define inversesqrt rsqrt\n"
    "constexpr sampler smp(coord::normalized, address::clamp_to_edge, filter::linear);\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex VOut vs(uint vid [[vertex_id]]) {\n"
    "    float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "    VOut o;\n"
    "    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "    o.uv = float2(p.x, 1.0 - p.y);\n"
    "    return o;\n"
    "}\n";

static int find_bind(const struct apple_pass_source *pass, const char *name, size_t len)
{
    for (int i = 0; i < pass->num_binds; i++) {
        if (pass->bind_lens[i] == len && !memcmp(pass->binds[i], name, len))
            return i;
    }
    return -1;
}

static void put_accessors(struct buf *b, const char *name, size_t len, int slot)
{
    put(b, "    float4 %.*s_tex(float2 p) const { return t%d.sample(smp, p); }\n",
        (int)len, name, slot);
    put(b, "    float4 %.*s_texOff(float2 o) const { return t%d.sample(smp, uv + o / s%d); }\n",
        (int)len, name, slot, slot);
}

static void put_macros(struct buf *b, const char *name, size_t len, int slot)
{
    put(b, "#define %.*s_pos uv\n", (int)len, name);
    put(b, "#define %.*s_pt (float2(1.0) / s%d)\n", (int)len, name, slot);
    put(b, "#define %.*s_size s%d\n", (int)len, name, slot);
}

char *apple_shader_msl(const struct apple_pass_source *pass)
{
    if (pass->num_binds < 1)
        return NULL;

    struct buf b = {0};
    put(&b, "%s", prelude);

    put(&b, "struct Pass {\n    float2 uv;\n");
    for (int i = 0; i < pass->num_binds; i++)
        put(&b, "    texture2d<float> t%d;\n    float2 s%d;\n", i, i);
    put(&b, "    Pass(float2 uv_");
    for (int i = 0; i < pass->num_binds; i++)
        put(&b, ", texture2d<float> t%d_, float2 s%d_", i, i);
    put(&b, ") : uv(uv_)");
    for (int i = 0; i < pass->num_binds; i++)
        put(&b, ", t%d(t%d_), s%d(s%d_)", i, i, i, i);
    put(&b, " {}\n");

    int hooked_slot = find_bind(pass, "HOOKED", 6);
    int named_slot = find_bind(pass, pass->hooked, pass->hooked_len);
    for (int i = 0; i < pass->num_binds; i++)
        put_accessors(&b, pass->binds[i], pass->bind_lens[i], i);
    if (hooked_slot >= 0 && named_slot < 0)
        put_accessors(&b, pass->hooked, pass->hooked_len, hooked_slot);
    if (named_slot >= 0 && hooked_slot < 0)
        put_accessors(&b, "HOOKED", 6, named_slot);

    for (int i = 0; i < pass->num_binds; i++)
        put_macros(&b, pass->binds[i], pass->bind_lens[i], i);
    if (hooked_slot >= 0 && named_slot < 0)
        put_macros(&b, pass->hooked, pass->hooked_len, hooked_slot);
    if (named_slot >= 0 && hooked_slot < 0)
        put_macros(&b, "HOOKED", 6, named_slot);

    put_raw(&b, pass->body, pass->body_len);
    put(&b, "\n};\n");

    put(&b, "fragment float4 fs(VOut in [[stage_in]]");
    for (int i = 0; i < pass->num_binds; i++)
        put(&b, ", texture2d<float> a%d [[texture(%d)]]", i, i);
    put(&b, ", constant float2 *sizes [[buffer(0)]]) {\n    Pass p(in.uv");
    for (int i = 0; i < pass->num_binds; i++)
        put(&b, ", a%d, sizes[%d]", i, i);
    put(&b, ");\n    return p.hook();\n}\n");

    return b.data;
}
