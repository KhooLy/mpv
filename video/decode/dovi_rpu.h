#pragma once

#include <stddef.h>
#include <stdint.h>

struct AVBufferRef;
struct AVDynamicHDRPlus;
struct mp_dovi_rpu;

struct mp_dovi_rpu *mp_dovi_rpu_create(void *ta_parent);
struct AVBufferRef *mp_dovi_rpu_parse(struct mp_dovi_rpu *s,
                                      const uint8_t *nal, size_t len,
                                      const char **err);

bool mp_dovi_rpu_l1(const uint8_t *nal, size_t len, float *peak, float *avg,
                    float *max_luma);
struct AVDynamicHDRPlus *mp_dovi_hdr10p(float peak, float avg, float max_luma,
                                        size_t *size);
