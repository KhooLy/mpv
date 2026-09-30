#include <stdbool.h>
#include <math.h>
#include <string.h>

#include <libavutil/buffer.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/mem.h>
#include <libdovi/rpu_parser.h>
#include <libplacebo/colorspace.h>

#include "common/common.h"
#include "mpv_talloc.h"
#include "dovi_rpu.h"

#define MAX_VDR_ID 16

struct mp_dovi_rpu {
    AVDOVIDataMapping vdr[MAX_VDR_ID];
    bool vdr_valid[MAX_VDR_ID];
    AVDOVIColorMetadata color;
};

static const AVDOVIColorMetadata color_default = {
    .ycc_to_rgb_matrix = {
        {9575, 8192}, {0, 8192}, {14742, 8192},
        {9575, 8192}, {1754, 8192}, {4383, 8192},
        {9575, 8192}, {17372, 8192}, {0, 8192},
    },
    .ycc_to_rgb_offset = {{1, 4}, {2, 1}, {2, 1}},
    .rgb_to_lms_matrix = {
        {5845, 16384}, {9702, 16384}, {837, 16384},
        {2568, 16384}, {12256, 16384}, {1561, 16384},
        {0, 16384}, {679, 16384}, {15705, 16384},
    },
    .signal_eotf = 39322,
    .signal_eotf_param0 = 15867,
    .signal_eotf_param1 = 228,
    .signal_eotf_param2 = 1383604,
    .signal_bit_depth = 14,
    .signal_full_range_flag = 1,
    .source_min_pq = 62,
    .source_max_pq = 3696,
    .source_diagonal = 42,
};

struct mp_dovi_rpu *mp_dovi_rpu_create(void *ta_parent)
{
    struct mp_dovi_rpu *s = talloc_zero(ta_parent, struct mp_dovi_rpu);
    s->color = color_default;
    return s;
}

static int64_t coef(const DoviRpuDataHeader *hdr, int64_t ipart, uint64_t fpart)
{
    int d = hdr->coefficient_log2_denom;
    if (hdr->coefficient_data_type) {
        union { uint32_t u; float f; } v = { .u = fpart };
        return v.f * (1LL << d);
    }
    return ipart * (1LL << d) + (int64_t)fpart;
}

static void map_curves(const DoviRpuDataHeader *hdr, const DoviRpuDataMapping *src,
                       AVDOVIDataMapping *dst)
{
    for (int c = 0; c < 3; c++) {
        const DoviReshapingCurve *in = &src->curves[c];
        AVDOVIReshapingCurve *out = &dst->curves[c];
        out->num_pivots = MPMIN(in->pivots.len, AV_DOVI_MAX_PIECES + 1);
        int pivot = 0;
        for (int i = 0; i < out->num_pivots; i++) {
            pivot += in->pivots.data[i];
            out->pivots[i] = MPMIN(pivot, UINT16_MAX);
        }
        const DoviPolynomialCurve *poly = in->polynomial;
        const DoviMMRCurve *mmr = in->mmr;
        for (int i = 0; i < out->num_pivots - 1; i++) {
            if (mmr && i < mmr->mmr_order_minus1.len) {
                out->mapping_idc[i] = AV_DOVI_MAPPING_MMR;
                out->mmr_order[i] = mmr->mmr_order_minus1.data[i] + 1;
                out->mmr_constant[i] = coef(hdr, mmr->mmr_constant_int.data[i],
                                            mmr->mmr_constant.data[i]);
                const DoviI64Data2D *ci = mmr->mmr_coef_int.list[i];
                const DoviU64Data2D *cf = mmr->mmr_coef.list[i];
                for (int j = 0; j < out->mmr_order[i]; j++) {
                    for (int k = 0; k < 7; k++) {
                        out->mmr_coef[i][j][k] = coef(hdr, ci->list[j]->data[k],
                                                      cf->list[j]->data[k]);
                    }
                }
            } else if (poly && i < poly->poly_order_minus1.len) {
                out->mapping_idc[i] = AV_DOVI_MAPPING_POLYNOMIAL;
                out->poly_order[i] = poly->poly_order_minus1.data[i] + 1;
                for (int k = 0; k <= out->poly_order[i]; k++) {
                    out->poly_coef[i][k] = coef(hdr, poly->poly_coef_int.list[i]->data[k],
                                                poly->poly_coef.list[i]->data[k]);
                }
            }
        }
    }
}

static void map_nlq(const DoviRpuDataHeader *hdr, const DoviRpuDataMapping *src,
                    AVDOVIDataMapping *dst)
{
    if (!src->nlq || src->nlq_method_idc < 0) {
        dst->nlq_method_idc = AV_DOVI_NLQ_NONE;
        return;
    }
    dst->nlq_method_idc = src->nlq_method_idc;
    int pivot = 0;
    for (int i = 0; i < 2 && i < src->nlq_pred_pivot_value.len; i++) {
        pivot += src->nlq_pred_pivot_value.data[i];
        dst->nlq_pivots[i] = MPMIN(pivot, UINT16_MAX);
    }
    for (int c = 0; c < 3; c++) {
        const DoviRpuDataNlq *n = src->nlq;
        AVDOVINLQParams *p = &dst->nlq[c];
        p->nlq_offset = n->nlq_offset[c];
        p->vdr_in_max = coef(hdr, n->vdr_in_max_int[c], n->vdr_in_max[c]);
        p->linear_deadzone_slope = coef(hdr, n->linear_deadzone_slope_int[c],
                                        n->linear_deadzone_slope[c]);
        p->linear_deadzone_threshold = coef(hdr, n->linear_deadzone_threshold_int[c],
                                            n->linear_deadzone_threshold[c]);
    }
}

static void map_color(const DoviVdrDmData *dm, int profile, AVDOVIColorMetadata *c)
{
    c->dm_metadata_id = dm->affected_dm_metadata_id;
    c->scene_refresh_flag = dm->scene_refresh_flag;
    if (dm->compressed)
        return;
    const int16_t ycc[9] = {
        dm->ycc_to_rgb_coef0, dm->ycc_to_rgb_coef1, dm->ycc_to_rgb_coef2,
        dm->ycc_to_rgb_coef3, dm->ycc_to_rgb_coef4, dm->ycc_to_rgb_coef5,
        dm->ycc_to_rgb_coef6, dm->ycc_to_rgb_coef7, dm->ycc_to_rgb_coef8,
    };
    const int16_t lms[9] = {
        dm->rgb_to_lms_coef0, dm->rgb_to_lms_coef1, dm->rgb_to_lms_coef2,
        dm->rgb_to_lms_coef3, dm->rgb_to_lms_coef4, dm->rgb_to_lms_coef5,
        dm->rgb_to_lms_coef6, dm->rgb_to_lms_coef7, dm->rgb_to_lms_coef8,
    };
    const uint32_t off[3] = {
        dm->ycc_to_rgb_offset0, dm->ycc_to_rgb_offset1, dm->ycc_to_rgb_offset2,
    };
    for (int i = 0; i < 9; i++) {
        c->ycc_to_rgb_matrix[i] = av_make_q(ycc[i], 1 << 13);
        c->rgb_to_lms_matrix[i] = av_make_q(lms[i], 1 << 14);
    }
    for (int i = 0; i < 3; i++) {
        int denom = profile == 4 ? 1 << 30 : 1 << 28;
        uint32_t o = off[i];
        if (o > INT32_MAX) {
            o >>= 1;
            denom >>= 1;
        }
        c->ycc_to_rgb_offset[i] = av_make_q(o, denom);
    }
    c->signal_eotf = dm->signal_eotf;
    c->signal_eotf_param0 = dm->signal_eotf_param0;
    c->signal_eotf_param1 = dm->signal_eotf_param1;
    c->signal_eotf_param2 = dm->signal_eotf_param2;
    c->signal_bit_depth = dm->signal_bit_depth;
    c->signal_color_space = dm->signal_color_space;
    c->signal_chroma_format = dm->signal_chroma_format;
    c->signal_full_range_flag = dm->signal_full_range_flag;
    c->source_min_pq = dm->source_min_pq;
    c->source_max_pq = dm->source_max_pq;
    c->source_diagonal = dm->source_diagonal;
}

static void free_metadata(void *opaque, uint8_t *data)
{
    av_free(data);
}

struct AVBufferRef *mp_dovi_rpu_parse(struct mp_dovi_rpu *s,
                                      const uint8_t *nal, size_t len,
                                      const char **err)
{
    AVBufferRef *buf = NULL;
    const DoviRpuDataHeader *hdr = NULL;
    const DoviRpuDataMapping *map = NULL;
    const DoviVdrDmData *dm = NULL;

    DoviRpuOpaque *rpu = dovi_parse_unspec62_nalu(nal, len);
    *err = dovi_rpu_get_error(rpu);
    if (*err)
        goto done;
    hdr = dovi_rpu_get_header(rpu);
    if (!hdr) {
        *err = "missing header";
        goto done;
    }

    int id = hdr->use_prev_vdr_rpu_flag ? hdr->prev_vdr_rpu_id : -1;
    map = dovi_rpu_get_data_mapping(rpu);
    if (id < 0) {
        if (!map) {
            *err = "missing data mapping";
            goto done;
        }
        id = map->vdr_rpu_id;
        if (id >= MAX_VDR_ID) {
            *err = "vdr_rpu_id out of range";
            goto done;
        }
        AVDOVIDataMapping *m = &s->vdr[id];
        *m = (AVDOVIDataMapping){
            .vdr_rpu_id = id,
            .mapping_color_space = map->mapping_color_space,
            .mapping_chroma_format_idc = map->mapping_chroma_format_idc,
            .num_x_partitions = map->num_x_partitions_minus1 + 1,
            .num_y_partitions = map->num_y_partitions_minus1 + 1,
        };
        map_curves(hdr, map, m);
        map_nlq(hdr, map, m);
        s->vdr_valid[id] = true;
    } else if (id >= MAX_VDR_ID || !s->vdr_valid[id]) {
        id = 0;
    }
    if (!s->vdr_valid[id]) {
        *err = "unknown previous vdr_rpu_id";
        goto done;
    }

    dm = hdr->vdr_dm_metadata_present_flag ? dovi_rpu_get_vdr_dm_data(rpu) : NULL;
    if (dm)
        map_color(dm, hdr->guessed_profile, &s->color);

    size_t size;
    AVDOVIMetadata *md = av_dovi_metadata_alloc(&size);
    if (!md) {
        *err = "out of memory";
        goto done;
    }
    AVDOVIRpuDataHeader *h = av_dovi_get_header(md);
    *h = (AVDOVIRpuDataHeader){
        .rpu_type = hdr->rpu_type,
        .rpu_format = hdr->rpu_format,
        .vdr_rpu_profile = hdr->vdr_rpu_profile,
        .vdr_rpu_level = hdr->vdr_rpu_level,
        .chroma_resampling_explicit_filter_flag = hdr->chroma_resampling_explicit_filter_flag,
        .coef_data_type = 0,
        .coef_log2_denom = hdr->coefficient_log2_denom,
        .vdr_rpu_normalized_idc = hdr->vdr_rpu_normalized_idc,
        .bl_video_full_range_flag = hdr->bl_video_full_range_flag,
        .bl_bit_depth = hdr->bl_bit_depth_minus8 + 8,
        .el_bit_depth = (hdr->el_bit_depth_minus8 & 0xff) + 8,
        .vdr_bit_depth = hdr->vdr_bit_depth_minus8 + 8,
        .spatial_resampling_filter_flag = hdr->spatial_resampling_filter_flag,
        .el_spatial_resampling_filter_flag = hdr->el_spatial_resampling_filter_flag,
        .disable_residual_flag = hdr->disable_residual_flag,
        .ext_mapping_idc_0_4 = (hdr->el_bit_depth_minus8 >> 8) & 0x1f,
        .ext_mapping_idc_5_7 = (hdr->el_bit_depth_minus8 >> 8) >> 5,
    };
    *av_dovi_get_mapping(md) = s->vdr[id];
    *av_dovi_get_color(md) = s->color;

    if (dm && dm->dm_data.level1) {
        AVDOVIDmData *l1 = av_dovi_get_ext(md, 0);
        l1->level = 1;
        l1->l1.min_pq = dm->dm_data.level1->min_pq;
        l1->l1.max_pq = dm->dm_data.level1->max_pq;
        l1->l1.avg_pq = dm->dm_data.level1->avg_pq;
        md->num_ext_blocks = 1;
    }

    buf = av_buffer_create((uint8_t *)md, size, free_metadata, NULL, 0);
    if (!buf) {
        av_free(md);
        *err = "out of memory";
    }

done:
    if (dm)
        dovi_rpu_free_vdr_dm_data(dm);
    if (map)
        dovi_rpu_free_data_mapping(map);
    if (hdr)
        dovi_rpu_free_header(hdr);
    dovi_rpu_free(rpu);
    return buf;
}

static float pq_nits(int pq)
{
    return pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, pq / 4095.0f);
}

bool mp_dovi_rpu_l1(const uint8_t *nal, size_t len, float *peak, float *avg,
                    float *max_luma)
{
    DoviRpuOpaque *rpu = dovi_parse_unspec62_nalu(nal, len);
    const DoviVdrDmData *dm = NULL;
    if (!dovi_rpu_get_error(rpu))
        dm = dovi_rpu_get_vdr_dm_data(rpu);
    bool ok = dm && dm->dm_data.level1 && dm->dm_data.level1->max_pq;
    if (ok) {
        *peak = pq_nits(dm->dm_data.level1->max_pq);
        *avg = pq_nits(dm->dm_data.level1->avg_pq);
        *max_luma = pq_nits(dm->source_max_pq);
    }
    if (dm)
        dovi_rpu_free_vdr_dm_data(dm);
    dovi_rpu_free(rpu);
    return ok;
}

AVDynamicHDRPlus *mp_dovi_hdr10p(float peak, float avg, float max_luma,
                                 size_t *size)
{
    AVDynamicHDRPlus *d = av_dynamic_hdr_plus_alloc(size);
    if (!d)
        return NULL;
    d->itu_t_t35_country_code = 0xB5;
    d->application_version = 1;
    d->num_windows = 1;
    d->targeted_system_display_maximum_luminance =
        av_make_q(lrintf(max_luma > 0 ? max_luma : 1000), 1);
    AVHDRPlusColorTransformParams *p = &d->params[0];
    for (int i = 0; i < 3; i++)
        p->maxscl[i] = av_make_q(lrintf(peak * 10), 100000);
    p->average_maxrgb = av_make_q(lrintf(avg * 10), 100000);
    static const uint8_t pct[] = {1, 5, 10, 25, 50, 75, 90, 95, 99};
    static const float scale[] = {0.05, 0.15, 0.3, 0.6, 0.9, 1.3, 2.2, 3.2, 6};
    p->num_distribution_maxrgb_percentiles = MP_ARRAY_SIZE(pct);
    for (int i = 0; i < MP_ARRAY_SIZE(pct); i++) {
        p->distribution_maxrgb[i].percentage = pct[i];
        p->distribution_maxrgb[i].percentile =
            av_make_q(lrintf(MPMIN(avg * scale[i], peak) * 10), 100000);
    }
    p->fraction_bright_pixels = av_make_q(0, 1000);
    return d;
}
