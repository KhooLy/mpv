#pragma once

#include <stddef.h>

struct apple_pass_source {
    const char *body;
    size_t body_len;
    const char *const *binds;
    size_t *bind_lens;
    int num_binds;
    const char *hooked;
    size_t hooked_len;
};

// Returns a malloc'd Metal source with entry points "vs" and "fs", or NULL.
char *apple_shader_msl(const struct apple_pass_source *pass);
