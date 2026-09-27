#pragma once

#include <stdbool.h>

struct MPContext;
struct mpv_node;

void mp_thumbnails_start(struct MPContext *mpctx);
void mp_thumbnails_stop(struct MPContext *mpctx);
void mp_thumbnails_uninit(struct MPContext *mpctx);
void mp_thumbnails_update(struct MPContext *mpctx);
bool mp_thumbnails_info(struct MPContext *mpctx, struct mpv_node *res);
void cmd_thumbnail(void *p);
