#pragma once

#include <stdbool.h>

struct mpv_global;
struct mp_log;
struct apple_shader_chain;

struct apple_shader_chain *apple_shader_chain_create(struct mpv_global *global,
                                                     struct mp_log *log,
                                                     char **paths);
bool apple_shader_chain_ready(struct apple_shader_chain *chain);

// pixbuf is a CVPixelBufferRef. done is a block taking the CVPixelBufferRef result.
bool apple_shader_chain_run(struct apple_shader_chain *chain, void *pixbuf,
                            int out_w, int out_h, void *done);

void apple_shader_chain_destroy(struct apple_shader_chain *chain);
