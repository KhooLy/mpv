#include "common/common.h"
#include "stream_bstr.h"

static const char shader0[] =
#include "third_party/shaders/anime4k_Anime4K_AutoDownscalePre_x2_glsl.inc"
;
static const char shader1[] =
#include "third_party/shaders/anime4k_Anime4K_AutoDownscalePre_x4_glsl.inc"
;
static const char shader2[] =
#include "third_party/shaders/anime4k_Anime4K_Clamp_Highlights_glsl.inc"
;
static const char shader3[] =
#include "third_party/shaders/anime4k_Anime4K_Deblur_DoG_glsl.inc"
;
static const char shader4[] =
#include "third_party/shaders/anime4k_Anime4K_Deblur_Original_glsl.inc"
;
static const char shader5[] =
#include "third_party/shaders/anime4k_Anime4K_Restore_CNN_M_glsl.inc"
;
static const char shader6[] =
#include "third_party/shaders/anime4k_Anime4K_Restore_CNN_S_glsl.inc"
;
static const char shader7[] =
#include "third_party/shaders/anime4k_Anime4K_Restore_CNN_Soft_M_glsl.inc"
;
static const char shader8[] =
#include "third_party/shaders/anime4k_Anime4K_Restore_CNN_Soft_S_glsl.inc"
;
static const char shader9[] =
#include "third_party/shaders/anime4k_Anime4K_Restore_CNN_Soft_VL_glsl.inc"
;
static const char shader10[] =
#include "third_party/shaders/anime4k_Anime4K_Restore_CNN_VL_glsl.inc"
;
static const char shader11[] =
#include "third_party/shaders/anime4k_Anime4K_Thin_Fast_glsl.inc"
;
static const char shader12[] =
#include "third_party/shaders/anime4k_Anime4K_Thin_HQ_glsl.inc"
;
static const char shader13[] =
#include "third_party/shaders/anime4k_Anime4K_Thin_VeryFast_glsl.inc"
;
static const char shader14[] =
#include "third_party/shaders/anime4k_Anime4K_Upscale_CNN_x2_M_glsl.inc"
;
static const char shader15[] =
#include "third_party/shaders/anime4k_Anime4K_Upscale_CNN_x2_S_glsl.inc"
;
static const char shader16[] =
#include "third_party/shaders/anime4k_Anime4K_Upscale_CNN_x2_VL_glsl.inc"
;
static const char shader17[] =
#include "third_party/shaders/anime4k_Anime4K_Upscale_Denoise_CNN_x2_M_glsl.inc"
;
static const char shader18[] =
#include "third_party/shaders/anime4k_Anime4K_Upscale_Denoise_CNN_x2_S_glsl.inc"
;
static const char shader19[] =
#include "third_party/shaders/anime4k_Anime4K_Upscale_Denoise_CNN_x2_VL_glsl.inc"
;
static const char shader20[] =
#include "third_party/shaders/artcnn_ArtCNN_C4F16_DS_glsl.inc"
;
static const char shader21[] =
#include "third_party/shaders/artcnn_ArtCNN_C4F32_DS_glsl.inc"
;

static const struct {
    const char *name;
    const char *data;
    size_t size;
} files[] = {
    {"Anime4K_AutoDownscalePre_x2.glsl", shader0, sizeof(shader0) - 1},
    {"Anime4K_AutoDownscalePre_x4.glsl", shader1, sizeof(shader1) - 1},
    {"Anime4K_Clamp_Highlights.glsl", shader2, sizeof(shader2) - 1},
    {"Anime4K_Deblur_DoG.glsl", shader3, sizeof(shader3) - 1},
    {"Anime4K_Deblur_Original.glsl", shader4, sizeof(shader4) - 1},
    {"Anime4K_Restore_CNN_M.glsl", shader5, sizeof(shader5) - 1},
    {"Anime4K_Restore_CNN_S.glsl", shader6, sizeof(shader6) - 1},
    {"Anime4K_Restore_CNN_Soft_M.glsl", shader7, sizeof(shader7) - 1},
    {"Anime4K_Restore_CNN_Soft_S.glsl", shader8, sizeof(shader8) - 1},
    {"Anime4K_Restore_CNN_Soft_VL.glsl", shader9, sizeof(shader9) - 1},
    {"Anime4K_Restore_CNN_VL.glsl", shader10, sizeof(shader10) - 1},
    {"Anime4K_Thin_Fast.glsl", shader11, sizeof(shader11) - 1},
    {"Anime4K_Thin_HQ.glsl", shader12, sizeof(shader12) - 1},
    {"Anime4K_Thin_VeryFast.glsl", shader13, sizeof(shader13) - 1},
    {"Anime4K_Upscale_CNN_x2_M.glsl", shader14, sizeof(shader14) - 1},
    {"Anime4K_Upscale_CNN_x2_S.glsl", shader15, sizeof(shader15) - 1},
    {"Anime4K_Upscale_CNN_x2_VL.glsl", shader16, sizeof(shader16) - 1},
    {"Anime4K_Upscale_Denoise_CNN_x2_M.glsl", shader17, sizeof(shader17) - 1},
    {"Anime4K_Upscale_Denoise_CNN_x2_S.glsl", shader18, sizeof(shader18) - 1},
    {"Anime4K_Upscale_Denoise_CNN_x2_VL.glsl", shader19, sizeof(shader19) - 1},
    {"ArtCNN_C4F16_DS.glsl", shader20, sizeof(shader20) - 1},
    {"ArtCNN_C4F32_DS.glsl", shader21, sizeof(shader21) - 1},
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
