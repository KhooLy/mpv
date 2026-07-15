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

#include <libplacebo/options.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/icc.h>
#include <libplacebo/utils/frame_queue.h>
#include <libplacebo/utils/libav.h>
#include <libplacebo/vulkan.h>

#include "common/common.h"
#include "options/m_config.h"
#include "options/path.h"
#include "stream/stream.h"
#include "sub/draw_bmp.h"
#include "sub/osd.h"
#include "video/mp_image.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/gpu/video.h"
#include "video/out/gpu/video_shaders.h"
#include "video/out/gpu_next/context.h"
#include "video/out/libmpv.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"
#include "mpv/render_vk.h"

#define MAX_GPU_NEXT_OSD_PARTS 64

struct osd_entry {
    pl_tex tex;
    struct pl_overlay_part *parts;
    int num_parts;
};

struct overlay_state {
    struct osd_entry entries[MAX_GPU_NEXT_OSD_PARTS];
    struct pl_overlay overlays[MAX_GPU_NEXT_OSD_PARTS];
};

struct scaler_params {
    struct pl_filter_config config;
};

struct user_hook {
    char *path;
    const struct pl_hook *hook;
};

struct priv {
    struct mpv_global *global;
    struct mp_log *log;

    pl_log pllog;
    pl_vulkan vk;
    pl_gpu gpu;
    pl_renderer rr;
    pl_queue queue;
    pl_options pars;
    struct scaler_params scalers[SCALER_COUNT];

    struct ra_ctx *ra_ctx; // minimal, headless: only .ra/.global/.log are set
    struct ra_hwdec_ctx hwdec_ctx;
    struct ra_hwdec_mapper *hwdec_mapper;
    struct ra_hwdec *hwdec;

    pl_tex *sub_tex;
    int num_sub_tex;
    pl_fmt osd_fmt[SUBBITMAP_COUNT];
    struct overlay_state overlays;
    struct osd_state *osd;
    struct vo *vo;
    struct mp_image_params target_params;
    struct mp_rect src, dst;
    struct mp_osd_res osd_res;

    pl_icc_object icc_profile;
    struct pl_icc_params icc_params;

    struct m_config_cache *opts_cache;
    struct m_config_cache *next_opts_cache;
    struct gl_next_opts *next_opts;

    struct user_hook *user_hooks;
    int num_user_hooks;
    const struct pl_hook **hooks;

    uint64_t last_id;
    double last_pts;
    bool want_reset;
    bool flush_cache;
};

static void update_lut(struct priv *p, struct user_lut *lut);

static bool format_supported(struct priv *p, int imgfmt, bool use_uint)
{
    struct pl_plane_data data[4] = {0};
    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(imgfmt);
    if (!desc.num_planes || !(desc.flags & MP_IMGFLAG_HAS_COMPS))
        return false;
    if (desc.flags & (MP_IMGFLAG_PAL | MP_IMGFLAG_HWACCEL))
        return false;
    if (!(desc.flags & MP_IMGFLAG_NE))
        return false;
    if ((desc.flags & MP_IMGFLAG_TYPE_FLOAT) && (desc.flags & MP_IMGFLAG_YUV))
        return false;

    for (int n = 0; n < desc.num_planes; n++) {
        if (desc.bpp[n] % 8)
            return false;
        data[n].pixel_stride = desc.bpp[n] / 8;
        data[n].type = (desc.flags & MP_IMGFLAG_TYPE_FLOAT)
                            ? PL_FMT_FLOAT
                            : (use_uint ? PL_FMT_UINT : PL_FMT_UNORM);
        int num_comps = 0;
        for (int c = 0; c < mp_imgfmt_desc_get_num_comps(&desc); c++) {
            if (desc.comps[c].plane != n)
                continue;
            data[n].component_size[num_comps++] = desc.comps[c].size;
        }
        if (!pl_plane_find_fmt(p->gpu, NULL, &data[n]))
            return false;
    }
    return true;
}

static bool check_format(struct render_backend *ctx, int imgfmt)
{
    struct priv *p = ctx->priv;
    if (ra_hwdec_get(&p->hwdec_ctx, imgfmt))
        return true;
    return format_supported(p, imgfmt, false) || format_supported(p, imgfmt, true);
}

static int plane_data_from_imgfmt(struct pl_plane_data out_data[4],
                                  struct pl_bit_encoding *out_bits,
                                  enum mp_imgfmt imgfmt)
{
    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(imgfmt);
    if (!desc.num_planes || !(desc.flags & MP_IMGFLAG_HAS_COMPS))
        return 0;
    if (desc.flags & (MP_IMGFLAG_HWACCEL | MP_IMGFLAG_PAL))
        return 0;
    if (!(desc.flags & MP_IMGFLAG_NE))
        return 0;
    if ((desc.flags & MP_IMGFLAG_TYPE_FLOAT) && (desc.flags & MP_IMGFLAG_YUV))
        return 0;

    bool has_bits = false;
    for (int p = 0; p < desc.num_planes; p++) {
        struct pl_plane_data *data = &out_data[p];
        struct mp_imgfmt_comp_desc sorted[MP_NUM_COMPONENTS];
        int num_comps = 0;
        if (desc.bpp[p] % 8)
            return 0;

        for (int c = 0; c < mp_imgfmt_desc_get_num_comps(&desc); c++) {
            if (desc.comps[c].plane != p)
                continue;
            data->component_map[num_comps] = c;
            sorted[num_comps] = desc.comps[c];
            num_comps++;
            for (int i = num_comps - 1; i > 0; i--) {
                if (sorted[i].offset >= sorted[i - 1].offset)
                    break;
                MPSWAP(struct mp_imgfmt_comp_desc, sorted[i], sorted[i - 1]);
                MPSWAP(int, data->component_map[i], data->component_map[i - 1]);
            }
        }

        uint64_t total_bits = 0;
        memset(data->component_size, 0, sizeof(data->component_size));
        for (int c = 0; c < num_comps; c++) {
            data->component_size[c] = sorted[c].size;
            data->component_pad[c] = sorted[c].offset - total_bits;
            total_bits += data->component_pad[c] + data->component_size[c];

            if (!out_bits || data->component_map[c] == PL_CHANNEL_A)
                continue;
            struct pl_bit_encoding bits = {
                .sample_depth = data->component_size[c],
                .color_depth = sorted[c].size - abs(sorted[c].pad),
                .bit_shift = MPMAX(sorted[c].pad, 0),
            };
            if (!has_bits) {
                *out_bits = bits;
                has_bits = true;
            } else if (!pl_bit_encoding_equal(out_bits, &bits)) {
                *out_bits = (struct pl_bit_encoding){0};
                out_bits = NULL;
            }
        }

        data->pixel_stride = desc.bpp[p] / 8;
        data->type = (desc.flags & MP_IMGFLAG_TYPE_FLOAT) ? PL_FMT_FLOAT : PL_FMT_UNORM;
    }

    return desc.num_planes;
}

static pl_tex wrap_hwdec_tex(struct ra_tex *ratex)
{
    return (pl_tex)ratex->priv;
}

static bool hwdec_acquire(pl_gpu gpu, struct pl_frame *frame)
{
    struct mp_image *mpi = frame->user_data;
    struct priv *p = mpi->priv;
    if (!p->hwdec_mapper || !mp_image_params_static_equal(&mpi->params, &p->hwdec_mapper->src_params)) {
        ra_hwdec_mapper_free(&p->hwdec_mapper);
        p->hwdec_mapper = ra_hwdec_mapper_create(p->hwdec, &mpi->params);
        if (!p->hwdec_mapper) {
            MP_ERR(p, "Initializing texture for hardware decoding failed.\n");
            return false;
        }
    }

    if (ra_hwdec_mapper_map(p->hwdec_mapper, mpi) < 0) {
        MP_ERR(p, "Mapping hardware decoded surface failed.\n");
        return false;
    }

    for (int n = 0; n < frame->num_planes; n++) {
        if (!(frame->planes[n].texture = wrap_hwdec_tex(p->hwdec_mapper->tex[n])))
            return false;
    }
    return true;
}

static void hwdec_release(pl_gpu gpu, struct pl_frame *frame)
{
    struct mp_image *mpi = frame->user_data;
    struct priv *p = mpi->priv;
    ra_hwdec_mapper_unmap(p->hwdec_mapper);
}

static bool map_frame(pl_gpu gpu, pl_tex *tex, const struct pl_source_frame *src,
                      struct pl_frame *frame)
{
    struct mp_image *mpi = src->frame_data;
    struct priv *p = mpi->priv;
    struct mp_image_params par = mpi->params;
    mp_image_params_guess_csp(&par);

    *frame = (struct pl_frame){
        .color = par.color,
        .repr = par.repr,
        .profile = {
            .data = mpi->icc_profile ? mpi->icc_profile->data : NULL,
            .len = mpi->icc_profile ? mpi->icc_profile->size : 0,
        },
        .rotation = par.rotate / 90,
        .user_data = mpi,
    };

    p->hwdec = ra_hwdec_get(&p->hwdec_ctx, mpi->imgfmt);
    if (p->hwdec) {
        struct mp_image_params dst_par = par;
        if (p->hwdec_mapper && mp_image_params_static_equal(&par, &p->hwdec_mapper->src_params)) {
            dst_par = p->hwdec_mapper->dst_params;
        } else {
            struct ra_hwdec_mapper *probe = ra_hwdec_mapper_create(p->hwdec, &par);
            if (!probe) {
                MP_ERR(p, "Failed probing hwdec frame format!\n");
                return false;
            }
            dst_par = probe->dst_params;
            ra_hwdec_mapper_free(&probe);
        }
        struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(dst_par.imgfmt);
        frame->acquire = hwdec_acquire;
        frame->release = hwdec_release;
        frame->num_planes = desc.num_planes;
        for (int n = 0; n < frame->num_planes; n++) {
            struct pl_plane *plane = &frame->planes[n];
            for (int c = 0; c < mp_imgfmt_desc_get_num_comps(&desc); c++) {
                if (desc.comps[c].plane != n)
                    continue;
                plane->component_mapping[plane->components++] = c;
            }
        }
        return true;
    }

    struct pl_plane_data data[4] = {0};
    bool use_uint = !format_supported(p, mpi->imgfmt, false);
    frame->num_planes = plane_data_from_imgfmt(data, &frame->repr.bits, mpi->imgfmt);
    if (use_uint) {
        for (int n = 0; n < frame->num_planes; n++)
            data[n].type = PL_FMT_UINT;
    }
    for (int n = 0; n < frame->num_planes; n++) {
        struct pl_plane *plane = &frame->planes[n];
        data[n].width = mp_image_plane_w(mpi, n);
        data[n].height = mp_image_plane_h(mpi, n);
        if (mpi->stride[n] < 0) {
            data[n].pixels = mpi->planes[n] + (data[n].height - 1) * mpi->stride[n];
            data[n].row_stride = -mpi->stride[n];
            plane->flipped = true;
        } else {
            data[n].pixels = mpi->planes[n];
            data[n].row_stride = mpi->stride[n];
        }
        if (!pl_upload_plane(gpu, plane, &tex[n], &data[n])) {
            MP_ERR(p, "Failed uploading frame!\n");
            return false;
        }
    }
    pl_frame_set_chroma_location(frame, par.chroma_location);
    if (mpi->film_grain)
        pl_film_grain_from_av(&frame->film_grain, (AVFilmGrainParams *)mpi->film_grain->data);

    update_lut(p, &p->next_opts->image_lut);
    frame->lut = p->next_opts->image_lut.lut;
    frame->lut_type = p->next_opts->image_lut.type;
    return true;
}

static void unmap_frame(pl_gpu gpu, struct pl_frame *frame,
                        const struct pl_source_frame *src)
{
    talloc_free((struct mp_image *)src->frame_data);
}

static void discard_frame(const struct pl_source_frame *src)
{
    talloc_free((struct mp_image *)src->frame_data);
}

static void update_overlays(struct priv *p, struct mp_osd_res res, struct mp_image *src,
                            int osd_flags, struct pl_frame *frame)
{
    if (!p->osd)
        return;
    struct sub_bitmap_list *subs = osd_render(p->osd, res, src ? src->pts : 0, osd_flags,
                                              mp_draw_sub_formats);
    frame->overlays = p->overlays.overlays;
    frame->num_overlays = 0;

    for (int n = 0; n < subs->num_items; n++) {
        const struct sub_bitmaps *item = subs->items[n];
        if (!item->num_parts || !item->packed)
            continue;
        struct osd_entry *entry = &p->overlays.entries[item->render_index];
        pl_fmt tex_fmt = p->osd_fmt[item->format];
        if (!entry->tex)
            MP_TARRAY_POP(p->sub_tex, p->num_sub_tex, &entry->tex);
        if (!pl_tex_recreate(p->gpu, &entry->tex, &(struct pl_tex_params){
                .format = tex_fmt,
                .w = MPMAX(item->packed_w, entry->tex ? entry->tex->params.w : 0),
                .h = MPMAX(item->packed_h, entry->tex ? entry->tex->params.h : 0),
                .host_writable = true,
                .sampleable = true,
            }))
        {
            MP_ERR(p, "Failed recreating OSD texture!\n");
            break;
        }
        if (!pl_tex_upload(p->gpu, &(struct pl_tex_transfer_params){
                .tex = entry->tex,
                .rc = {.x1 = item->packed_w, .y1 = item->packed_h},
                .row_pitch = item->packed->stride[0],
                .ptr = item->packed->planes[0],
            }))
        {
            MP_ERR(p, "Failed uploading OSD texture!\n");
            break;
        }

        entry->num_parts = 0;
        for (int i = 0; i < item->num_parts; i++) {
            const struct sub_bitmap *b = &item->parts[i];
            if (b->dw == 0 || b->dh == 0)
                continue;
            uint32_t c = b->libass.color;
            struct pl_overlay_part part = {
                .src = {b->src_x, b->src_y, b->src_x + b->w, b->src_y + b->h},
                .dst = {b->x, b->y, b->x + b->dw, b->y + b->dh},
                .color = {
                    (c >> 24) / 255.0f,
                    ((c >> 16) & 0xFF) / 255.0f,
                    ((c >> 8) & 0xFF) / 255.0f,
                    (255 - (c & 0xFF)) / 255.0f,
                },
            };
            MP_TARRAY_APPEND(p, entry->parts, entry->num_parts, part);
        }

        struct pl_overlay *ol = &p->overlays.overlays[frame->num_overlays++];
        *ol = (struct pl_overlay){
            .tex = entry->tex,
            .parts = entry->parts,
            .num_parts = entry->num_parts,
            .color = pl_color_space_srgb,
            .coords = PL_OVERLAY_COORDS_DST_FRAME,
        };
        if (item->format == SUBBITMAP_BGRA) {
            ol->mode = PL_OVERLAY_NORMAL;
            ol->repr.alpha = PL_ALPHA_PREMULTIPLIED;
        } else {
            ol->mode = PL_OVERLAY_MONOCHROME;
            ol->repr.alpha = PL_ALPHA_INDEPENDENT;
        }
    }

    talloc_free(subs);
}

static void update_lut(struct priv *p, struct user_lut *lut)
{
    if (!lut->opt || !lut->opt[0]) {
        pl_lut_free(&lut->lut);
        TA_FREEP(&lut->path);
        return;
    }
    if (lut->path && strcmp(lut->path, lut->opt) == 0)
        return;

    pl_lut_free(&lut->lut);
    talloc_replace(p, lut->path, lut->opt);

    char *fname = mp_get_user_path(NULL, p->global, lut->path);
    MP_VERBOSE(p, "Loading custom LUT '%s'\n", fname);
    const int lut_max_size = 1536 << 20;
    struct bstr lutdata = stream_read_file(fname, NULL, p->global, lut_max_size);
    if (!lutdata.len) {
        MP_ERR(p, "Failed to read LUT data from %s, make sure it's a valid file "
                  "and smaller or equal to %d bytes\n", fname, lut_max_size);
    } else {
        lut->lut = pl_lut_parse_cube(p->pllog, lutdata.start, lutdata.len);
    }
    talloc_free(fname);
    talloc_free(lutdata.start);
}

static const struct pl_hook *load_hook(struct priv *p, const char *path)
{
    if (!path || !path[0])
        return NULL;

    for (int i = 0; i < p->num_user_hooks; i++) {
        if (strcmp(p->user_hooks[i].path, path) == 0)
            return p->user_hooks[i].hook;
    }

    char *fname = mp_get_user_path(NULL, p->global, path);
    bstr shader = stream_read_file(fname, p, p->global, 1000000000); // 1GB
    talloc_free(fname);

    const struct pl_hook *hook = NULL;
    if (shader.len)
        hook = pl_mpv_user_shader_parse(p->gpu, shader.start, shader.len);

    MP_TARRAY_APPEND(p, p->user_hooks, p->num_user_hooks, (struct user_hook){
        .path = talloc_strdup(p, path),
        .hook = hook,
    });

    return hook;
}

static void update_hook_opts_dynamic(struct priv *p, const struct pl_hook *hook,
                                     const struct mp_image *mpi)
{
    for (int i = 0; i < hook->num_parameters; i++) {
        double val;
        const struct pl_hook_par *hp = &hook->parameters[i];
        if (!gpu_get_auto_param(mpi, bstr0(hp->name), &val))
            continue;
        switch (hp->type) {
        case PL_VAR_FLOAT: hp->data->f = val; break;
        case PL_VAR_SINT:  hp->data->i = lrint(val); break;
        case PL_VAR_UINT:  hp->data->u = lrint(val); break;
        }
    }
}

static void update_hook_opts(struct priv *p, char **opts, const char *shaderpath,
                             const struct pl_hook *hook)
{
    for (int i = 0; i < hook->num_parameters; i++) {
        const struct pl_hook_par *hp = &hook->parameters[i];
        memcpy(hp->data, &hp->initial, sizeof(*hp->data));
    }
    if (!opts)
        return;

    const char *basename = mp_basename(shaderpath);
    struct bstr shadername;
    if (!mp_splitext(basename, &shadername))
        shadername = bstr0(basename);

    for (int n = 0; opts[n * 2]; n++) {
        struct bstr k = bstr0(opts[n * 2 + 0]);
        struct bstr v = bstr0(opts[n * 2 + 1]);
        int pos;
        if ((pos = bstrchr(k, '/')) >= 0) {
            if (!bstr_equals(bstr_splice(k, 0, pos), shadername))
                continue;
            k = bstr_cut(k, pos + 1);
        }

        for (int i = 0; i < hook->num_parameters; i++) {
            const struct pl_hook_par *hp = &hook->parameters[i];
            if (!bstr_equals0(k, hp->name) != 0)
                continue;

            m_option_t opt = {.name = hp->name};
            if (hp->names) {
                for (int j = hp->minimum.i; j <= hp->maximum.i; j++) {
                    if (bstr_equals0(v, hp->names[j])) {
                        hp->data->i = j;
                        goto next_hook;
                    }
                }
            }
            switch (hp->type) {
            case PL_VAR_FLOAT:
                opt.type = &m_option_type_float;
                opt.min = hp->minimum.f;
                opt.max = hp->maximum.f;
                break;
            case PL_VAR_SINT:
                opt.type = &m_option_type_int;
                opt.min = hp->minimum.i;
                opt.max = hp->maximum.i;
                break;
            case PL_VAR_UINT:
                opt.type = &m_option_type_int;
                opt.min = MPMIN(hp->minimum.u, INT_MAX);
                opt.max = MPMIN(hp->maximum.u, INT_MAX);
                break;
            }
            if (!opt.type)
                goto next_hook;
            opt.type->parse(p->log, &opt, k, v, hp->data);
            goto next_hook;
        }
    next_hook:;
    }
}

static void apply_crop(struct pl_frame *frame, struct mp_rect crop, int width, int height)
{
    frame->crop = (struct pl_rect2df){
        .x0 = crop.x0, .y0 = crop.y0, .x1 = crop.x1, .y1 = crop.y1,
    };
    pl_rect2df_rotate(&frame->crop, -frame->rotation);
    if (frame->crop.x1 < frame->crop.x0) {
        frame->crop.x0 = width - frame->crop.x0;
        frame->crop.x1 = width - frame->crop.x1;
    }
    if (frame->crop.y1 < frame->crop.y0) {
        frame->crop.y0 = height - frame->crop.y0;
        frame->crop.y1 = height - frame->crop.y1;
    }
}

static void apply_target_contrast(struct priv *p, struct pl_color_space *color)
{
    const struct gl_video_opts *opts = p->opts_cache->opts;

    if (!opts->target_contrast) {
        color->hdr.min_luma = 0;
        return;
    }
    if (opts->target_contrast == -1) {
        color->hdr.min_luma = 1e-7;
        return;
    }
    pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
        .color = color,
        .metadata = PL_HDR_METADATA_HDR10,
        .scaling = PL_HDR_NITS,
        .out_max = &color->hdr.max_luma
    ));
    color->hdr.min_luma = color->hdr.max_luma / opts->target_contrast;
}

static void apply_target_color(struct priv *p, struct pl_frame *target)
{
    const struct gl_video_opts *opts = p->opts_cache->opts;

    update_lut(p, &p->next_opts->target_lut);
    target->lut = p->next_opts->target_lut.lut;
    target->lut_type = p->next_opts->target_lut.type;
    target->icc = p->icc_profile; // MPV_RENDER_PARAM_ICC_PROFILE, if the embedder set one

    target->color = pl_color_space_srgb; // sane default absent any --target-* opts
    if (opts->target_prim)
        target->color.primaries = opts->target_prim;
    if (opts->target_trc)
        target->color.transfer = opts->target_trc;
    if (opts->target_peak)
        target->color.hdr.max_luma = opts->target_peak;
    if (opts->hdr_reference_white && !pl_color_transfer_is_hdr(target->color.transfer))
        target->color.hdr.max_luma = opts->hdr_reference_white;
    apply_target_contrast(p, &target->color);
    if (opts->target_gamut)
        mp_parse_raw_primaries(mp_null_log, opts->target_gamut, &target->color.hdr.prim);

    if (opts->dither_depth > 0) {
        target->repr.bits.color_depth = opts->dither_depth;
        target->repr.bits.sample_depth = opts->dither_depth;
    }
}

static const struct pl_filter_config *map_scaler(struct priv *p, enum scaler_unit unit)
{
    const struct pl_filter_preset fixed_scalers[] = {
        { "bilinear",       &pl_filter_bilinear },
        { "bicubic_fast",   &pl_filter_bicubic },
        { "nearest",        &pl_filter_nearest },
        { "oversample",     &pl_filter_oversample },
        {0},
    };
    const struct pl_filter_preset fixed_frame_mixers[] = {
        { "linear",         &pl_filter_bilinear },
        { "oversample",     &pl_filter_oversample },
        {0},
    };
    const struct pl_filter_preset *fixed_presets =
        unit == SCALER_TSCALE ? fixed_frame_mixers : fixed_scalers;

    const struct gl_video_opts *opts = p->opts_cache->opts;
    const struct scaler_config *cfg = &opts->scaler[unit];
    struct scaler_config tmp;
    if (cfg->kernel.function == SCALER_INHERIT) {
        tmp = *cfg;
        scaler_conf_merge(&tmp, &opts->scaler[SCALER_SCALE], unit);
        cfg = &tmp;
    }

    const char *kernel_name = m_opt_choice_str(cfg->kernel.functions, cfg->kernel.function);
    for (int i = 0; fixed_presets[i].name; i++) {
        if (strcmp(kernel_name, fixed_presets[i].name) == 0)
            return fixed_presets[i].filter;
    }

    struct scaler_params *par = &p->scalers[unit];
    const struct pl_filter_preset *preset;
    const struct pl_filter_function_preset *fpreset;
    if ((preset = pl_find_filter_preset(kernel_name))) {
        par->config = *preset->filter;
    } else if ((fpreset = pl_find_filter_function_preset(kernel_name))) {
        par->config = (struct pl_filter_config){
            .kernel = fpreset->function,
            .params[0] = fpreset->function->params[0],
            .params[1] = fpreset->function->params[1],
        };
    } else {
        MP_ERR(p, "Failed mapping filter function '%s', no libplacebo analog?\n", kernel_name);
        return &pl_filter_bilinear;
    }

    const struct pl_filter_function_preset *wpreset;
    if ((wpreset = pl_find_filter_function_preset(
             m_opt_choice_str(cfg->window.functions, cfg->window.function)))) {
        par->config.window = wpreset->function;
        par->config.wparams[0] = wpreset->function->params[0];
        par->config.wparams[1] = wpreset->function->params[1];
    }

    for (int i = 0; i < 2; i++) {
        if (!isnan(cfg->kernel.params[i]))
            par->config.params[i] = cfg->kernel.params[i];
        if (!isnan(cfg->window.params[i]))
            par->config.wparams[i] = cfg->window.params[i];
    }

    par->config.clamp = cfg->clamp;
    if (cfg->antiring > 0.0)
        par->config.antiring = cfg->antiring;
    if (cfg->kernel.blur > 0.0)
        par->config.blur = cfg->kernel.blur;
    if (cfg->kernel.taper > 0.0)
        par->config.taper = cfg->kernel.taper;
    if (cfg->radius > 0.0) {
        if (par->config.kernel->resizable) {
            par->config.radius = cfg->radius;
        } else {
            MP_WARN(p, "Filter radius specified but filter '%s' is not resizable, ignoring\n",
                    kernel_name);
        }
    }

    return &par->config;
}

static void update_render_options(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    pl_options pars = p->pars;
    const struct gl_video_opts *opts = p->opts_cache->opts;

    pars->params.skip_anti_aliasing = !opts->correct_downscaling;
    pars->params.disable_linear_scaling = !opts->linear_downscaling && !opts->linear_upscaling;
    pars->params.disable_fbos = opts->dumb_mode == 1;

    pars->params.upscaler = map_scaler(p, SCALER_SCALE);
    pars->params.downscaler = map_scaler(p, SCALER_DSCALE);
    pars->params.plane_upscaler = map_scaler(p, SCALER_CSCALE);
    pars->params.frame_mixer = opts->interpolation ? map_scaler(p, SCALER_TSCALE) : NULL;

    update_lut(p, &p->next_opts->lut);
    pars->params.lut = p->next_opts->lut.lut;
    pars->params.lut_type = p->next_opts->lut.type;
    for (char **kv = p->next_opts->raw_opts; kv && kv[0]; kv += 2)
        pl_options_set_str(pars, kv[0], kv[1]);

    pars->params.num_hooks = 0;
    const struct pl_hook *hook;
    for (int i = 0; opts->user_shaders && opts->user_shaders[i]; i++) {
        if ((hook = load_hook(p, opts->user_shaders[i]))) {
            MP_TARRAY_APPEND(p, p->hooks, pars->params.num_hooks, hook);
            update_hook_opts(p, opts->user_shader_opts, opts->user_shaders[i], hook);
        }
    }
    pars->params.hooks = p->hooks;

    pars->params.deband_params = opts->deband ? &pars->deband_params : NULL;
    pars->deband_params.iterations = opts->deband_opts->iterations;
    pars->deband_params.radius = opts->deband_opts->range;
    pars->deband_params.threshold = opts->deband_opts->threshold / 16.384;
    pars->deband_params.grain = opts->deband_opts->grain / 8.192;

    pars->params.sigmoid_params = opts->sigmoid_upscaling ? &pars->sigmoid_params : NULL;
    pars->sigmoid_params.center = opts->sigmoid_center;
    pars->sigmoid_params.slope = opts->sigmoid_slope;

    pars->params.peak_detect_params = opts->tone_map.compute_peak >= 0 ? &pars->peak_detect_params : NULL;
    pars->peak_detect_params.smoothing_period = opts->tone_map.decay_rate;
    pars->peak_detect_params.scene_threshold_low = opts->tone_map.scene_threshold_low;
    pars->peak_detect_params.scene_threshold_high = opts->tone_map.scene_threshold_high;
    pars->peak_detect_params.percentile = opts->tone_map.peak_percentile;
    pars->peak_detect_params.allow_delayed = false;

    const struct pl_tone_map_function * const tone_map_funs[] = {
        [TONE_MAPPING_AUTO]      = &pl_tone_map_auto,
        [TONE_MAPPING_CLIP]      = &pl_tone_map_clip,
        [TONE_MAPPING_MOBIUS]    = &pl_tone_map_mobius,
        [TONE_MAPPING_REINHARD]  = &pl_tone_map_reinhard,
        [TONE_MAPPING_HABLE]     = &pl_tone_map_hable,
        [TONE_MAPPING_GAMMA]     = &pl_tone_map_gamma,
        [TONE_MAPPING_LINEAR]    = &pl_tone_map_linear,
        [TONE_MAPPING_SPLINE]    = &pl_tone_map_spline,
        [TONE_MAPPING_BT_2390]   = &pl_tone_map_bt2390,
        [TONE_MAPPING_BT_2446A]  = &pl_tone_map_bt2446a,
        [TONE_MAPPING_ST2094_40] = &pl_tone_map_st2094_40,
        [TONE_MAPPING_ST2094_10] = &pl_tone_map_st2094_10,
    };
    const struct pl_gamut_map_function * const gamut_modes[] = {
        [GAMUT_AUTO]       = NULL,
        [GAMUT_CLIP]       = &pl_gamut_map_clip,
        [GAMUT_PERCEPTUAL] = &pl_gamut_map_perceptual,
        [GAMUT_RELATIVE]   = &pl_gamut_map_relative,
        [GAMUT_SATURATION] = &pl_gamut_map_saturation,
        [GAMUT_ABSOLUTE]   = &pl_gamut_map_absolute,
        [GAMUT_DESATURATE] = &pl_gamut_map_desaturate,
        [GAMUT_DARKEN]     = &pl_gamut_map_darken,
        [GAMUT_WARN]       = &pl_gamut_map_highlight,
        [GAMUT_LINEAR]     = &pl_gamut_map_linear,
    };

    pars->color_map_params.tone_mapping_function = tone_map_funs[opts->tone_map.curve];
    pars->color_map_params.tone_mapping_param = opts->tone_map.curve_param;
    if (isnan(pars->color_map_params.tone_mapping_param))
        pars->color_map_params.tone_mapping_param = 0.0;
    pars->color_map_params.inverse_tone_mapping = opts->tone_map.inverse;
    pars->color_map_params.contrast_recovery = opts->tone_map.contrast_recovery;
    pars->color_map_params.visualize_lut = opts->tone_map.visualize;
    pars->color_map_params.contrast_smoothness = opts->tone_map.contrast_smoothness;
    pars->color_map_params.gamut_mapping = opts->tone_map.gamut_mode == GAMUT_AUTO
        ? pl_color_map_default_params.gamut_mapping
        : gamut_modes[opts->tone_map.gamut_mode];

    pars->params.dither_params = NULL;
    pars->params.error_diffusion = NULL;
    switch (opts->dither_algo) {
    case DITHER_ERROR_DIFFUSION:
        pars->params.error_diffusion = pl_find_error_diffusion_kernel(opts->error_diffusion);
        if (!pars->params.error_diffusion) {
            MP_WARN(p, "Could not find error diffusion kernel '%s', falling "
                    "back to fruit.\n", opts->error_diffusion);
        }
        MP_FALLTHROUGH;
    case DITHER_ORDERED:
    case DITHER_FRUIT:
        pars->params.dither_params = &pars->dither_params;
        pars->dither_params.method = opts->dither_algo == DITHER_ORDERED
                                ? PL_DITHER_ORDERED_FIXED
                                : PL_DITHER_BLUE_NOISE;
        pars->dither_params.lut_size = opts->dither_size;
        pars->dither_params.temporal = opts->temporal_dither;
        break;
    }
    if (opts->dither_depth < 0) {
        pars->params.dither_params = NULL;
        pars->params.error_diffusion = NULL;
    }
}

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;
    p->global = ctx->global;
    p->log = ctx->log;

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

    p->vk = pl_vulkan_import(p->pllog, pl_vulkan_import_params(
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
    if (!p->vk)
        return MPV_ERROR_UNSUPPORTED;
    p->gpu = p->vk->gpu;

    p->ra_ctx = talloc_zero(p, struct ra_ctx);
    p->ra_ctx->ra = ra_create_pl(p->gpu, ctx->log);
    p->ra_ctx->global = ctx->global;
    p->ra_ctx->log = ctx->log;
    if (!p->ra_ctx->ra)
        return MPV_ERROR_UNSUPPORTED;

    p->rr = pl_renderer_create(p->pllog, p->gpu);
    p->queue = pl_queue_create(p->gpu);
    p->pars = pl_options_alloc(p->pllog);
    p->osd_fmt[SUBBITMAP_LIBASS] = pl_find_named_fmt(p->gpu, "r8");
    p->osd_fmt[SUBBITMAP_BGRA] = pl_find_named_fmt(p->gpu, "bgra8");

    p->opts_cache = m_config_cache_alloc(p, ctx->global, &gl_video_conf);
    p->next_opts_cache = m_config_cache_alloc(p, ctx->global, &gl_next_conf);
    p->next_opts = p->next_opts_cache->opts;
    update_render_options(ctx);

    ctx->hwdec_devs = hwdec_devices_create();
    p->hwdec_ctx = (struct ra_hwdec_ctx){
        .log = ctx->log,
        .global = ctx->global,
        .ra_ctx = p->ra_ctx,
    };
    ra_hwdec_ctx_init(&p->hwdec_ctx, ctx->hwdec_devs, NULL, true);

    ctx->driver_caps = VO_CAP_ROTATE90 | VO_CAP_VFLIP;
    return 0;
}

static int set_parameter(struct render_backend *ctx, mpv_render_param param)
{
    struct priv *p = ctx->priv;
    switch (param.type) {
    case MPV_RENDER_PARAM_ICC_PROFILE: {
        mpv_byte_array *data = param.data;
        struct pl_icc_profile profile = {.data = data->data, .len = data->size};
        pl_icc_profile_compute_signature(&profile);
        pl_icc_update(p->pllog, &p->icc_profile, &profile, &p->icc_params);
        return 0;
    }
    default:
        return MPV_ERROR_NOT_IMPLEMENTED;
    }
}

static void reconfig(struct render_backend *ctx, struct mp_image_params *params)
{
}

static void reset(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    pl_renderer_flush_cache(p->rr);
}

static void update_external(struct render_backend *ctx, struct vo *vo)
{
    struct priv *p = ctx->priv;
    p->osd = vo ? vo->osd : NULL;
    p->vo = vo;
    if (vo) {
        int req_frames = 2;
        if (p->pars->params.frame_mixer) {
            req_frames += ceilf(p->pars->params.frame_mixer->kernel->radius) *
                          (p->pars->params.skip_anti_aliasing ? 1 : 2);
        }
        vo_set_queue_params(vo, 0, MPMIN(VO_MAX_REQ_FRAMES, req_frames));
    }
}

static void resize(struct render_backend *ctx, struct mp_rect *src,
                   struct mp_rect *dst, struct mp_osd_res *osd)
{
    struct priv *p = ctx->priv;
    p->src = *src;
    p->dst = *dst;
    p->osd_res = *osd;
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
    struct priv *p = ctx->priv;

    bool opts_changed = m_config_cache_update(p->opts_cache);
    opts_changed = m_config_cache_update(p->next_opts_cache) || opts_changed;
    if (opts_changed)
        update_render_options(ctx);

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

    struct pl_frame target = {
        .repr = pl_color_repr_rgb,
        .num_planes = 1,
        .planes[0] = {
            .texture = target_tex,
            .components = 4,
            .component_mapping = {0, 1, 2, 3},
        },
    };
    apply_target_color(p, &target);
    int dither_depth = GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_DEPTH, int, 0);
    if (dither_depth > 0) {
        target.repr.bits.color_depth = dither_depth;
        target.repr.bits.sample_depth = dither_depth;
    }
    const struct gl_video_opts *opts = p->opts_cache->opts;
    struct pl_render_params render_params = p->pars->params;
    bool can_interpolate = render_params.frame_mixer && frame->display_synced &&
                           !frame->still && frame->num_frames > 1;
    double pts_offset = can_interpolate ? frame->ideal_frame_vsync : 0;
    bool cache_frame = (frame->display_synced && frame->num_vsyncs > 1) || frame->still;
    render_params.skip_caching_single_frame = !cache_frame;
    render_params.preserve_mixing_cache = !frame->still;
    if (frame->still)
        render_params.frame_mixer = NULL;

    if (frame->current && frame->current->params.vflip) {
        pl_matrix2x2 m = {.m = {{1, 0}, {0, -1}}};
        p->pars->distort_params.transform.mat = m;
        render_params.distort_params = &p->pars->distort_params;
    } else {
        render_params.distort_params = NULL;
    }

    if (frame->current) {
        for (int i = 0; i < render_params.num_hooks; i++)
            update_hook_opts_dynamic(p, render_params.hooks[i], frame->current);
    }

    struct pl_source_frame vpts;
    if (frame->current && !p->want_reset) {
        if (pl_queue_peek(p->queue, 0, &vpts) &&
            frame->current->pts + MPMAX(0, pts_offset) < vpts.pts)
        {
            p->want_reset = true;
        }
    }

    for (int n = 0; n < frame->num_frames; n++) {
        uint64_t id = frame->frame_id + n;
        if (p->want_reset) {
            pl_queue_reset(p->queue);
            p->last_pts = 0.0;
            p->last_id = 0;
            p->want_reset = false;
            p->flush_cache = true;
        }
        if (p->flush_cache) {
            pl_renderer_flush_cache(p->rr);
            p->flush_cache = false;
        }
        if (id <= p->last_id)
            continue;
        struct mp_image *mpi = mp_image_new_ref(frame->frames[n]);
        mpi->priv = p;
        pl_queue_push(p->queue, &(struct pl_source_frame){
            .pts = mpi->pts,
            .duration = can_interpolate ? frame->approx_duration : 0,
            .frame_data = mpi,
            .map = map_frame,
            .unmap = unmap_frame,
            .discard = discard_frame,
        });
        p->last_id = id;
    }

    bool ok = true;
    struct pl_frame_mix mix = {0};
    if (frame->current) {
        struct pl_queue_params qparams = *pl_queue_params(
            .pts = frame->current->pts + pts_offset,
            .radius = pl_frame_mix_radius(&render_params),
            .vsync_duration = can_interpolate ? frame->ideal_frame_vsync_duration : 0,
            .interpolation_threshold = opts->interpolation_threshold,
            .drift_compensation = 0,
        );
        struct pl_source_frame first;
        if (pl_queue_peek(p->queue, 0, &first) && qparams.pts < first.pts)
            qparams.pts = first.pts;
        p->last_pts = qparams.pts;

        if (pl_queue_update(p->queue, &mix, &qparams) == PL_QUEUE_ERR) {
            MP_ERR(p, "Failed updating frames!\n");
            ok = false;
        } else if (p->vo && p->vo->params) {
            for (int i = 0; i < mix.num_frames; i++) {
                struct pl_frame *image = (struct pl_frame *)mix.frames[i];
                apply_crop(image, p->src, p->vo->params->w, p->vo->params->h);
            }
        }
    }

    apply_crop(&target, p->dst, img->w, img->h);

    if (ok) {
        update_overlays(p, p->osd_res, frame->current, 0, &target);
        ok = pl_render_image_mix(p->rr, &mix, &target, &render_params);
    }

    if (ok && p->vo) {
        struct pl_frame ref_frame;
        pl_frames_infer_mix(p->rr, &mix, &target, &ref_frame);

        mp_mutex_lock(&p->vo->params_mutex);
        p->target_params = (struct mp_image_params){
            .w = img->w,
            .h = img->h,
            .color = target.color,
            .repr = target.repr,
            .rotate = target.rotation,
        };
        p->vo->target_params = &p->target_params;
        if (p->vo->params) {
            p->vo->has_peak_detect_values =
                pl_renderer_get_hdr_metadata(p->rr, &p->vo->params->color.hdr);
        }
        mp_mutex_unlock(&p->vo->params_mutex);
    }

    if (!ok)
        pl_tex_clear(p->gpu, target_tex, (float[4]){0.5, 0.0, 1.0, 1.0});

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

static struct mp_image *get_image(struct render_backend *ctx, int imgfmt,
                                  int w, int h, int stride_align, int flags)
{
    return NULL; // advanced_control/DR not used by current libmpv embedders
}

static void screenshot(struct render_backend *ctx, struct vo_frame *frame,
                       struct voctrl_screenshot *args)
{
    args->res = NULL; // not implemented for this backend yet
}

static void perfdata(struct render_backend *ctx, struct voctrl_performance_data *out)
{
}

static void destroy(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;

    pl_queue_destroy(&p->queue);
    for (int i = 0; i < MP_ARRAY_SIZE(p->overlays.entries); i++)
        pl_tex_destroy(p->gpu, &p->overlays.entries[i].tex);
    for (int i = 0; i < p->num_sub_tex; i++)
        pl_tex_destroy(p->gpu, &p->sub_tex[i]);

    ra_hwdec_mapper_free(&p->hwdec_mapper);
    ra_hwdec_ctx_uninit(&p->hwdec_ctx);
    hwdec_devices_destroy(ctx->hwdec_devs);

    if (p->next_opts) {
        pl_lut_free(&p->next_opts->image_lut.lut);
        pl_lut_free(&p->next_opts->lut.lut);
        pl_lut_free(&p->next_opts->target_lut.lut);
    }
    for (int i = 0; i < p->num_user_hooks; i++)
        pl_mpv_user_shader_destroy(&p->user_hooks[i].hook);

    pl_icc_close(&p->icc_profile);
    pl_options_free(&p->pars);
    pl_renderer_destroy(&p->rr);
    if (p->ra_ctx && p->ra_ctx->ra) {
        p->ra_ctx->ra->fns->destroy(p->ra_ctx->ra);
        p->ra_ctx->ra = NULL;
    }
    pl_vulkan_destroy(&p->vk);
    if (p->pllog)
        pl_log_destroy(&p->pllog);
}

const struct render_backend_fns render_backend_gpu_next_vk = {
    .init = init,
    .check_format = check_format,
    .set_parameter = set_parameter,
    .reconfig = reconfig,
    .reset = reset,
    .update_external = update_external,
    .resize = resize,
    .get_target_size = get_target_size,
    .render = render,
    .get_image = get_image,
    .screenshot = screenshot,
    .perfdata = perfdata,
    .destroy = destroy,
};
