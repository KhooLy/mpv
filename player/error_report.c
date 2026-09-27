#include <stdio.h>
#include <string.h>

#include "common/common.h"
#include "common/msg.h"
#include "common/msg_control.h"
#include "misc/bstr.h"
#include "misc/ctype.h"
#include "misc/node.h"
#include "options/m_option.h"
#include "options/m_property.h"
#include "osdep/timer.h"
#include "command.h"
#include "core.h"

#define MAX_LINES 64

struct err_line {
    double time;
    int level;
    char *prefix;
    char *text;
};

struct error_report {
    struct mp_log_buffer *buf;
    double start;
    struct err_line *lines;
    int num_lines;
    struct mpv_node state;
    struct mpv_node last;
    bool have_last;
};

static const char *const state_props[] = {
    "file-format", "demuxer-via-network", "duration", "time-pos",
    "demuxer-cache-duration", "cache-speed", "paused-for-cache",
    "video-codec", "video-params", "hwdec-current", "audio-codec",
    "audio-params", "current-ao", "current-vo", "track-list/count",
    "estimated-vf-fps", "frame-drop-count", "decoder-frame-drop-count",
    "mpv-version", "ffmpeg-version",
    NULL
};

void error_report_init(struct MPContext *mpctx)
{
    struct error_report *r = talloc_zero(mpctx, struct error_report);
    r->buf = mp_msg_log_buffer_new(mpctx->global, MAX_LINES, MSGL_WARN,
                                   NULL, NULL);
    mpctx->error_report = r;
}

void error_report_uninit(struct MPContext *mpctx)
{
    struct error_report *r = mpctx->error_report;
    if (!r)
        return;
    mp_msg_log_buffer_destroy(r->buf);
    TA_FREEP(&mpctx->error_report);
}

static char *redact(void *ta, const char *text, size_t len)
{
    char *res = talloc_strndup(ta, text, len);
    char *p = res;
    while ((p = strstr(p, "://"))) {
        char *host = p + 3;
        char *end = host + strcspn(host, " \t\n'\"<>");
        char *at = memchr(host, '@', end - host);
        char *slash = memchr(host, '/', end - host);
        if (at && (!slash || at < slash)) {
            memmove(host, at + 1, strlen(at + 1) + 1);
            end -= at + 1 - host;
        }
        char *q = strpbrk(host, "?#");
        if (q && q < end)
            memmove(q, end, strlen(end) + 1);
        p = host;
    }
    return res;
}

static void drain(struct error_report *r)
{
    struct mp_log_buffer_entry *e;
    while ((e = mp_msg_log_buffer_read(r->buf))) {
        if (e->level > MSGL_WARN || !e->text || !e->text[0]) {
            talloc_free(e);
            continue;
        }
        if (r->num_lines == MAX_LINES) {
            talloc_free(r->lines[0].prefix);
            talloc_free(r->lines[0].text);
            memmove(&r->lines[0], &r->lines[1],
                    (MAX_LINES - 1) * sizeof(r->lines[0]));
            r->num_lines--;
        }
        size_t len = strlen(e->text);
        while (len && e->text[len - 1] == '\n')
            len--;
        MP_TARRAY_APPEND(r, r->lines, r->num_lines, (struct err_line){
            .time = mp_time_sec() - r->start,
            .level = e->level,
            .prefix = talloc_strdup(r, e->prefix),
            .text = redact(r, e->text, len),
        });
        talloc_free(e);
    }
}

void error_report_update(struct MPContext *mpctx)
{
    drain(mpctx->error_report);
}

void error_report_start(struct MPContext *mpctx)
{
    struct error_report *r = mpctx->error_report;
    drain(r);
    for (int n = 0; n < r->num_lines; n++) {
        talloc_free(r->lines[n].prefix);
        talloc_free(r->lines[n].text);
    }
    r->num_lines = 0;
    r->start = mp_time_sec();
    talloc_free(r->state.u.list);
    r->state = (struct mpv_node){0};
}

static void adopt(void *ta, struct mpv_node *n)
{
    if (n->format == MPV_FORMAT_STRING)
        talloc_steal(ta, n->u.string);
    if (n->format == MPV_FORMAT_NODE_MAP || n->format == MPV_FORMAT_NODE_ARRAY)
        talloc_steal(ta, n->u.list);
}

void error_report_snapshot(struct MPContext *mpctx)
{
    struct error_report *r = mpctx->error_report;
    talloc_free(r->state.u.list);
    node_init(&r->state, MPV_FORMAT_NODE_MAP, NULL);
    for (int n = 0; state_props[n]; n++) {
        struct mpv_node val;
        if (mp_property_do(state_props[n], M_PROPERTY_GET_NODE, &val, mpctx) <= 0)
            continue;
        struct mpv_node *dst = node_map_add(&r->state, state_props[n],
                                            MPV_FORMAT_NONE);
        *dst = val;
        adopt(r->state.u.list, dst);
    }
}

static bool has(const char *text, const char *needle)
{
    return bstr_find(bstr0(text), bstr0(needle)) >= 0;
}

static int http_status(const char *text)
{
    const char *p;
    int code;
    if ((p = strstr(text, "HTTP error ")) && sscanf(p + 11, "%d", &code) == 1)
        return code;
    if ((p = strstr(text, "Server returned ")) && sscanf(p + 16, "%d", &code) == 1)
        return code;
    if ((p = strstr(text, "HTTP/1.1 ")) && sscanf(p + 9, "%d", &code) == 1 &&
        code >= 400)
        return code;
    return 0;
}

static const char *classify(struct error_report *r, int error, int *status,
                            const char **message)
{
    const char *cat = NULL;
    *status = 0;
    *message = NULL;
    for (int n = r->num_lines - 1; n >= 0; n--) {
        const char *orig = r->lines[n].text;
        if (r->lines[n].level <= MSGL_ERR)
            *message = orig;
        if (!*status)
            *status = http_status(orig);
        char t[512];
        snprintf(t, sizeof(t), "%s", orig);
        for (char *c = t; *c; c++)
            *c = mp_tolower(*c);
        if (cat)
            continue;
        if (*status) {
            cat = "http";
        } else if (has(t, "certificate") || has(t, "tls") || has(t, "ssl") ||
                   has(t, "handshake")) {
            cat = "tls";
        } else if (has(t, "connection refused") || has(t, "timed out") ||
                   has(t, "connection reset") || has(t, "unreachable") ||
                   has(t, "failed to resolve") || has(t, "name or service") ||
                   has(t, "no route") || has(t, "could not connect") ||
                   has(t, "partial file") || has(t, "broken pipe") ||
                   has(t, "end of file") || has(t, "i/o error")) {
            cat = "network";
        } else if (has(t, "decod") || has(t, "codec")) {
            cat = "codec";
        }
    }
    if (!*message && r->num_lines)
        *message = r->lines[r->num_lines - 1].text;
    if (*status)
        return "http";
    if (cat)
        return cat;
    switch (error) {
    case MPV_ERROR_UNKNOWN_FORMAT: return "format";
    case MPV_ERROR_AO_INIT_FAILED: return "audio-output";
    case MPV_ERROR_VO_INIT_FAILED: return "video-output";
    case MPV_ERROR_NOTHING_TO_PLAY: return "no-streams";
    }
    return error < 0 ? "unknown" : "truncated";
}

static char *strip_url(void *ta, const char *url)
{
    if (!url)
        return NULL;
    char *res = talloc_strdup(ta, url);
    char *q = strpbrk(res, "?#");
    if (q)
        *q = '\0';
    char *scheme = strstr(res, "://");
    if (scheme) {
        char *host = scheme + 3;
        char *slash = strchr(host, '/');
        char *at = strchr(host, '@');
        if (at && (!slash || at < slash))
            memmove(host, at + 1, strlen(at + 1) + 1);
    }
    return res;
}

static void add_lines(struct error_report *r, struct mpv_node *arr)
{
    for (int n = 0; n < r->num_lines; n++) {
        struct mpv_node *e = node_array_add(arr, MPV_FORMAT_NODE_MAP);
        node_map_add_double(e, "time", r->lines[n].time);
        node_map_add_string(e, "level", mp_log_levels[r->lines[n].level]);
        node_map_add_string(e, "prefix", r->lines[n].prefix);
        node_map_add_string(e, "text", r->lines[n].text);
    }
}

static double state_num(struct error_report *r, const char *key)
{
    if (r->state.format != MPV_FORMAT_NODE_MAP)
        return -1;
    struct mpv_node *n = node_map_get(&r->state, key);
    if (n && n->format == MPV_FORMAT_DOUBLE)
        return n->u.double_;
    if (n && n->format == MPV_FORMAT_INT64)
        return n->u.int64;
    return -1;
}

void error_report_finish(struct MPContext *mpctx, int error, bool eof,
                         const char *url)
{
    struct error_report *r = mpctx->error_report;
    bool truncated = false;
    if (eof && error >= 0) {
        double pos = state_num(r, "time-pos");
        double dur = state_num(r, "duration");
        truncated = pos >= 0 && dur > 0 && pos < dur - 5;
    }
    if (error >= 0 && !truncated)
        return;
    drain(r);
    talloc_free(r->last.u.list);
    node_init(&r->last, MPV_FORMAT_NODE_MAP, NULL);
    int status;
    const char *message;
    const char *cat = classify(r, error, &status, &message);
    node_map_add_string(&r->last, "error", truncated ? "premature end of file" :
                                           mpv_error_string(error));
    node_map_add_int64(&r->last, "code", error);
    node_map_add_string(&r->last, "category", cat);
    if (status)
        node_map_add_int64(&r->last, "http-status", status);
    if (message)
        node_map_add_string(&r->last, "message", message);
    char *u = strip_url(NULL, url);
    if (u)
        node_map_add_string(&r->last, "url", u);
    talloc_free(u);
    node_map_add_double(&r->last, "elapsed", mp_time_sec() - r->start);
    node_map_add_flag(&r->last, "played", mpctx->shown_aframes ||
                                          mpctx->shown_vframes);
    if (r->state.format == MPV_FORMAT_NODE_MAP) {
        struct mpv_node *st = node_map_add(&r->last, "state", MPV_FORMAT_NONE);
        m_option_copy(&(struct m_option){.type = CONF_TYPE_NODE}, st, &r->state);
        adopt(r->last.u.list, st);
    }
    add_lines(r, node_map_add(&r->last, "log", MPV_FORMAT_NODE_ARRAY));
    r->have_last = true;
    mp_notify_property(mpctx, "last-error");
}

int error_report_property(struct MPContext *mpctx, bool last, int action,
                          void *arg)
{
    struct error_report *r = mpctx->error_report;
    if (last && !r->have_last)
        return M_PROPERTY_UNAVAILABLE;
    if (action == M_PROPERTY_GET_TYPE) {
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_NODE};
        return M_PROPERTY_OK;
    }
    if (action != M_PROPERTY_GET)
        return M_PROPERTY_NOT_IMPLEMENTED;
    struct mpv_node *res = arg;
    if (last) {
        m_option_copy(&(struct m_option){.type = CONF_TYPE_NODE}, res, &r->last);
        return M_PROPERTY_OK;
    }
    drain(r);
    node_init(res, MPV_FORMAT_NODE_ARRAY, NULL);
    add_lines(r, res);
    return M_PROPERTY_OK;
}
