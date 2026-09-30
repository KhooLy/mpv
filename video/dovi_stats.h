#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "osdep/threads.h"

struct mpv_global;
struct mpv_node;

#define DOVI_GPU_BINS 500

struct mp_dovi_stats {
    mp_mutex lock;

    char decoder[96];
    char path[16];
    char reason[128];
    char output[8];
    char el_type[4];
    char cm[8];
    char trims[128];
    char error[160];
    double error_pts;
    int profile, level, compat;
    float mastering_max, mastering_min, max_cll, max_fall;

    int64_t rpus, rpu_errors, scenes, converted, convert_errors;
    int64_t frames, missing, trim_frames;
    int64_t dropped_dec, dropped_vo;
    double l1_peak, l1_sum;
    int64_t l1_n;
    uint32_t gpu_hist[DOVI_GPU_BINS];
    int64_t gpu_n;
    double gpu_sum, gpu_max;

    float l1[3];
    char trim[32];
    float tm_src, tm_dst;
    float hdr10p[3];
    double gpu_recent;
};

void mp_dovi_stats_init(struct mpv_global *global);
void mp_dovi_stats_reset(struct mp_dovi_stats *s);
void mp_dovi_stats_error(struct mp_dovi_stats *s, double pts, const char *msg);
void mp_dovi_stats_gpu(struct mp_dovi_stats *s, double ms);
void mp_dovi_stats_live(struct mp_dovi_stats *s, struct mpv_node *res);
void mp_dovi_stats_summary(struct mp_dovi_stats *s, struct mpv_node *res);
