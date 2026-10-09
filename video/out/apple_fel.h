#pragma once

#include <stdbool.h>

struct mp_log;
struct pl_dovi_metadata;
struct apple_fel;

enum {
    APPLE_FEL_UNSUPPORTED = -1,
    APPLE_FEL_BUSY = 0,
    APPLE_FEL_QUEUED = 1,
};

struct apple_fel *apple_fel_create(struct mp_log *log, bool dv_domain);

// With dv_domain the output is 16-bit 4:2:2 biplanar holding the composed IPT
// signal, otherwise 10-bit BT.2020 PQ RGB.
// bl and el are CVPixelBufferRef, el may be NULL. done is a block taking the
// composed CVPixelBufferRef.
int apple_fel_compose(struct apple_fel *f, void *bl, void *el,
                      const struct pl_dovi_metadata *dovi, void *done);

void apple_fel_destroy(struct apple_fel *f);
