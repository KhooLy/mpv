/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>

#include <ass/ass.h>
#include <ass/ass_types.h>

#include "common/common.h"
#include "common/msg.h"
#include "options/path.h"
#include "ass_mp.h"
#include "osd.h"
#include "stream/stream.h"

#if HAVE_ANDROID
#include <dlfcn.h>
#endif

// res_y should be track->PlayResY
// It determines scaling of font sizes and more.
void mp_ass_set_style(ASS_Style *style, double res_y,
                      const struct osd_style_opts *opts)
{
    if (!style)
        return;

    if (opts->font) {
        if (!style->FontName || strcmp(style->FontName, opts->font) != 0) {
            free(style->FontName);
            style->FontName = strdup(opts->font);
        }
    }

    // libass_font_size = FontSize * (window_height / res_y)
    // scale translates parameters from PlayResY=720 to res_y
    double scale = res_y / 720.0;

    style->FontSize = opts->font_size * scale;
    style->PrimaryColour = MP_ASS_COLOR(opts->color);
    style->SecondaryColour = style->PrimaryColour;
    style->OutlineColour = MP_ASS_COLOR(opts->outline_color);
    style->BackColour = MP_ASS_COLOR(opts->back_color);
    style->BorderStyle = opts->border_style;
    style->Outline = opts->outline_size * scale;
    style->Shadow = opts->shadow_offset * scale;
    style->Spacing = opts->spacing * scale;
    style->MarginL = opts->margin_x * scale;
    style->MarginR = style->MarginL;
    style->MarginV = (opts->margin_y + opts->margin_y_offset) * scale;
    style->ScaleX = 1.;
    style->ScaleY = 1.;
    style->Alignment = 1 + (opts->align_x + 1) + (opts->align_y + 2) % 3 * 4;
#ifdef ASS_JUSTIFY_LEFT
    style->Justify = opts->justify;
#endif
    style->Blur = opts->blur;
    style->Bold = opts->bold;
    style->Italic = opts->italic;
}

#if HAVE_ANDROID
static char *android_default_font(void *ta_ctx)
{
    void *(*matcher_create)(void) = dlsym(RTLD_DEFAULT, "AFontMatcher_create");
    void *(*matcher_match)(void *, const char *, const uint16_t *, uint32_t,
                           uint32_t *) = dlsym(RTLD_DEFAULT, "AFontMatcher_match");
    void (*matcher_destroy)(void *) = dlsym(RTLD_DEFAULT, "AFontMatcher_destroy");
    const char *(*font_path)(void *) = dlsym(RTLD_DEFAULT, "AFont_getFontFilePath");
    void (*font_close)(void *) = dlsym(RTLD_DEFAULT, "AFont_close");

    char *path = NULL;
    if (matcher_create && matcher_match && matcher_destroy && font_path && font_close) {
        void *matcher = matcher_create();
        if (matcher) {
            const uint16_t text[] = {'a'};
            void *font = matcher_match(matcher, "sans-serif", text, 1, NULL);
            if (font) {
                path = talloc_strdup(ta_ctx, font_path(font));
                font_close(font);
            }
            matcher_destroy(matcher);
        }
    }
    if (!path || !mp_path_exists(path))
        path = talloc_strdup(ta_ctx, "/system/fonts/Roboto-Regular.ttf");
    return mp_path_exists(path) ? path : NULL;
}
#endif

void mp_ass_configure_fonts(ASS_Renderer *priv, struct osd_style_opts *opts,
                            struct mpv_global *global, struct mp_log *log)
{
    void *tmp = talloc_new(NULL);
    char *default_font = mp_find_config_file(tmp, global, "subfont.ttf");
    char *config       = mp_find_config_file(tmp, global, "fonts.conf");

    if (default_font && !mp_path_exists(default_font))
        default_font = NULL;
#if HAVE_ANDROID
    if (!default_font)
        default_font = android_default_font(tmp);
#endif

    int font_provider = ASS_FONTPROVIDER_AUTODETECT;
    if (opts->font_provider == 1)
        font_provider = ASS_FONTPROVIDER_NONE;
    if (opts->font_provider == 2)
        font_provider = ASS_FONTPROVIDER_FONTCONFIG;

    mp_verbose(log, "Setting up fonts...\n");
    ass_set_fonts(priv, default_font, opts->font, font_provider, config, 1);
    mp_verbose(log, "Done.\n");

    talloc_free(tmp);
}

static const int map_ass_level[] = {
    MSGL_ERR,           // 0 "FATAL errors"
    MSGL_WARN,
    MSGL_INFO,
    MSGL_V,
    MSGL_V,
    MSGL_DEBUG,         // 5 application recommended level
    MSGL_TRACE,
    MSGL_TRACE,         // 7 "verbose DEBUG"
};

MP_PRINTF_ATTRIBUTE(2, 0)
static void message_callback(int level, const char *format, va_list va, void *ctx)
{
    struct mp_log *log = ctx;
    if (!log)
        return;
    level = map_ass_level[level];
    mp_msg_va(log, level, format, va);
    // libass messages lack trailing \n
    mp_msg(log, level, "\n");
}

ASS_Library *mp_ass_init(struct mpv_global *global,
                         struct osd_style_opts *opts, struct mp_log *log)
{
    char *path = opts->fonts_dir && opts->fonts_dir[0] ?
                 mp_get_user_path(NULL, global, opts->fonts_dir) :
                 mp_find_config_file(NULL, global, "fonts");
    mp_dbg(log, "ASS library version: 0x%x (runtime 0x%x)\n",
           (unsigned)LIBASS_VERSION, ass_library_version());
    ASS_Library *priv = ass_library_init();
    if (!priv)
        abort();
    ass_set_message_cb(priv, message_callback, log);
    if (path)
        ass_set_fonts_dir(priv, path);
    talloc_free(path);
    return priv;
}

void mp_ass_flush_old_events(ASS_Track *track, long long ts)
{
    int n = 0;
    for (; n < track->n_events; n++) {
        if ((track->events[n].Start + track->events[n].Duration) >= ts)
            break;
        ass_free_event(track, n);
        track->n_events--;
    }
    for (int i = 0; n > 0 && i < track->n_events; i++) {
        track->events[i] = track->events[i+n];
    }
}

// Set *out_rc to [x0, y0, x1, y1] of the graphical bounding box in script
// coordinates.
// Set it to [inf, inf, -inf, -inf] if empty.
void mp_ass_get_bb(ASS_Image *image_list, ASS_Track *track,
                   struct mp_osd_res *res, double *out_rc)
{
    double rc[4] = {INFINITY, INFINITY, -INFINITY, -INFINITY};

    for (ASS_Image *img = image_list; img; img = img->next) {
        if (img->w == 0 || img->h == 0)
            continue;
        rc[0] = MPMIN(rc[0], img->dst_x);
        rc[1] = MPMIN(rc[1], img->dst_y);
        rc[2] = MPMAX(rc[2], img->dst_x + img->w);
        rc[3] = MPMAX(rc[3], img->dst_y + img->h);
    }

    double scale = track->PlayResY / (double)MPMAX(res->h, 1);
    if (scale > 0) {
        for (int i = 0; i < 4; i++)
            out_rc[i] = rc[i] * scale;
    }
}
