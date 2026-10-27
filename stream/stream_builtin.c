#include "common/common.h"
#include "stream_bstr.h"

static const char clamp_highlights[] =
#include "third_party/shaders/anime4k_Anime4K_Clamp_Highlights_glsl.inc"
;
static const char restore_cnn_m[] =
#include "third_party/shaders/anime4k_Anime4K_Restore_CNN_M_glsl.inc"
;
static const char restore_cnn_vl[] =
#include "third_party/shaders/anime4k_Anime4K_Restore_CNN_VL_glsl.inc"
;
static const char upscale_cnn_x2_s[] =
#include "third_party/shaders/anime4k_Anime4K_Upscale_CNN_x2_S_glsl.inc"
;
static const char upscale_cnn_x2_m[] =
#include "third_party/shaders/anime4k_Anime4K_Upscale_CNN_x2_M_glsl.inc"
;
static const char upscale_cnn_x2_vl[] =
#include "third_party/shaders/anime4k_Anime4K_Upscale_CNN_x2_VL_glsl.inc"
;
static const char auto_downscale_x2[] =
#include "third_party/shaders/anime4k_Anime4K_AutoDownscalePre_x2_glsl.inc"
;
static const char auto_downscale_x4[] =
#include "third_party/shaders/anime4k_Anime4K_AutoDownscalePre_x4_glsl.inc"
;
static const char artcnn_c4f16_ds[] =
#include "third_party/shaders/artcnn_ArtCNN_C4F16_DS_glsl.inc"
;
static const char artcnn_c4f32_ds[] =
#include "third_party/shaders/artcnn_ArtCNN_C4F32_DS_glsl.inc"
;

static const struct {
    const char *name;
    const char *data;
    size_t size;
} files[] = {
#define FILE(n, v) {n, v, sizeof(v) - 1}
    FILE("Anime4K_Clamp_Highlights.glsl", clamp_highlights),
    FILE("Anime4K_Restore_CNN_M.glsl", restore_cnn_m),
    FILE("Anime4K_Restore_CNN_VL.glsl", restore_cnn_vl),
    FILE("Anime4K_Upscale_CNN_x2_S.glsl", upscale_cnn_x2_s),
    FILE("Anime4K_Upscale_CNN_x2_M.glsl", upscale_cnn_x2_m),
    FILE("Anime4K_Upscale_CNN_x2_VL.glsl", upscale_cnn_x2_vl),
    FILE("Anime4K_AutoDownscalePre_x2.glsl", auto_downscale_x2),
    FILE("Anime4K_AutoDownscalePre_x4.glsl", auto_downscale_x4),
    FILE("ArtCNN_C4F16_DS.glsl", artcnn_c4f16_ds),
    FILE("ArtCNN_C4F32_DS.glsl", artcnn_c4f32_ds),
};

static int open2(stream_t *stream, const struct stream_open_args *args)
{
    bstr name = bstr0(stream->url);
    if (!bstr_eatstart0(&name, "builtin://"))
        return STREAM_NO_MATCH;

    for (int n = 0; n < MP_ARRAY_SIZE(files); n++) {
        if (bstr_equals0(name, files[n].name))
            return stream_bstr_open2(stream, args,
                                     bstrdup(stream, (bstr){(char *)files[n].data, files[n].size}));
    }

    MP_ERR(stream, "No builtin file '%.*s'.\n", BSTR_P(name));
    return STREAM_ERROR;
}

const stream_info_t stream_info_builtin = {
    .name = "builtin",
    .open2 = open2,
    .protocols = (const char*const[]){ "builtin", NULL },
    .local_fs = true,
};
