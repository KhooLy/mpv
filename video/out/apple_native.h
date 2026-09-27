#pragma once

#include "video/hwdec.h"

struct apple_native_sink {
    struct mp_hwdec_ctx hwctx;
    void *layer;
    void *timebase;
};
