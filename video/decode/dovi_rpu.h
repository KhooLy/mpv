#pragma once

#include <stddef.h>
#include <stdint.h>

struct AVBufferRef;
struct mp_dovi_rpu;

struct mp_dovi_rpu *mp_dovi_rpu_create(void *ta_parent);
struct AVBufferRef *mp_dovi_rpu_parse(struct mp_dovi_rpu *s,
                                      const uint8_t *nal, size_t len,
                                      const char **err);
