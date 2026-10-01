#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct AVBufferRef;
struct AVDynamicHDRPlus;
struct mp_dovi_rpu;
struct mp_dovi_stats;
typedef struct DoviRpuOpaque DoviRpuOpaque;

struct mp_dovi_rpu *mp_dovi_rpu_create(void *ta_parent, struct mp_dovi_stats *stats);
void mp_dovi_stats_rpu(struct mp_dovi_stats *stats, const DoviRpuOpaque *rpu);
struct AVBufferRef *mp_dovi_rpu_parse(struct mp_dovi_rpu *s,
                                      const uint8_t *nal, size_t len, bool obu,
                                      const char **err);

bool mp_dovi_rpu_l1(struct mp_dovi_stats *stats, const uint8_t *nal, size_t len,
                    float *peak, float *avg, float *max_luma);
struct AVDynamicHDRPlus *mp_dovi_hdr10p(float peak, float avg, float max_luma,
                                        const float pct[9], size_t *size);
