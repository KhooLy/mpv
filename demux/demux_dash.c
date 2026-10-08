#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/common.h"
#include "common/msg.h"
#include "misc/bstr.h"
#include "options/m_config.h"
#include "options/options.h"
#include "osdep/timer.h"
#include "stream/stream.h"

#include "demux.h"
#include "demux_adaptive.h"

struct xattr {
    char *key, *val;
};

struct xnode {
    char *name;
    struct xattr *attrs;
    int num_attr;
    struct xnode **kids;
    int num_kids;
    char *text;
};

struct xparser {
    void *ctx;
    bstr s;
    bool failed;
};

static char *xdecode(void *ctx, bstr in)
{
    char *out = talloc_size(ctx, in.len + 1);
    int o = 0;
    for (int i = 0; i < in.len;) {
        if (in.start[i] != '&') {
            out[o++] = in.start[i++];
            continue;
        }
        bstr rest = bstr_cut(in, i);
        int semi = bstrchr(rest, ';');
        if (semi < 0 || semi > 10) {
            out[o++] = in.start[i++];
            continue;
        }
        bstr ent = bstr_splice(rest, 1, semi);
        char c = 0;
        if (bstr_equals0(ent, "amp"))
            c = '&';
        else if (bstr_equals0(ent, "lt"))
            c = '<';
        else if (bstr_equals0(ent, "gt"))
            c = '>';
        else if (bstr_equals0(ent, "quot"))
            c = '"';
        else if (bstr_equals0(ent, "apos"))
            c = '\'';
        else if (ent.len > 1 && ent.start[0] == '#')
            c = (char)strtol(bstrto0(NULL, bstr_cut(ent, 1)), NULL, 10);
        if (!c) {
            out[o++] = in.start[i++];
            continue;
        }
        out[o++] = c;
        i += semi + 1;
    }
    out[o] = 0;
    return out;
}

static void xskip_ws(struct xparser *x)
{
    x->s = bstr_lstrip(x->s);
}

static bool xeat(struct xparser *x, const char *prefix)
{
    return bstr_eatstart0(&x->s, prefix);
}

static bool xskip_to(struct xparser *x, const char *end)
{
    int i = bstr_find0(x->s, end);
    if (i < 0) {
        x->failed = true;
        return false;
    }
    x->s = bstr_cut(x->s, i + strlen(end));
    return true;
}

static const char *xlocal(const char *name)
{
    const char *c = strchr(name, ':');
    return c ? c + 1 : name;
}

static struct xnode *xparse_element(struct xparser *x, int depth)
{
    if (depth > 64 || !xeat(x, "<")) {
        x->failed = true;
        return NULL;
    }
    struct xnode *n = talloc_zero(x->ctx, struct xnode);
    int len = 0;
    while (len < x->s.len && !isspace((unsigned char)x->s.start[len]) &&
           x->s.start[len] != '>' && x->s.start[len] != '/')
        len++;
    n->name = talloc_strdup(n, xlocal(bstrto0(n, bstr_splice(x->s, 0, len))));
    x->s = bstr_cut(x->s, len);

    for (;;) {
        xskip_ws(x);
        if (!x->s.len) {
            x->failed = true;
            return NULL;
        }
        if (xeat(x, "/>"))
            return n;
        if (xeat(x, ">"))
            break;
        int eq = bstrchr(x->s, '=');
        if (eq < 0) {
            x->failed = true;
            return NULL;
        }
        char *key = bstrto0(n, bstr_strip(bstr_splice(x->s, 0, eq)));
        x->s = bstr_cut(x->s, eq + 1);
        xskip_ws(x);
        if (!x->s.len || (x->s.start[0] != '"' && x->s.start[0] != '\'')) {
            x->failed = true;
            return NULL;
        }
        char q = x->s.start[0];
        x->s = bstr_cut(x->s, 1);
        int end = bstrchr(x->s, q);
        if (end < 0) {
            x->failed = true;
            return NULL;
        }
        char *val = xdecode(n, bstr_splice(x->s, 0, end));
        x->s = bstr_cut(x->s, end + 1);
        struct xattr a = {talloc_strdup(n, xlocal(key)), val};
        MP_TARRAY_APPEND(n, n->attrs, n->num_attr, a);
    }

    for (;;) {
        int lt = bstrchr(x->s, '<');
        if (lt < 0) {
            x->failed = true;
            return NULL;
        }
        bstr text = bstr_splice(x->s, 0, lt);
        if (bstr_strip(text).len && !n->text)
            n->text = xdecode(n, bstr_strip(text));
        x->s = bstr_cut(x->s, lt);
        if (xeat(x, "</")) {
            if (!xskip_to(x, ">"))
                return NULL;
            return n;
        }
        if (xeat(x, "<!--")) {
            if (!xskip_to(x, "-->"))
                return NULL;
            continue;
        }
        if (xeat(x, "<![CDATA[")) {
            int e = bstr_find0(x->s, "]]>");
            if (e < 0) {
                x->failed = true;
                return NULL;
            }
            if (!n->text)
                n->text = bstrto0(n, bstr_splice(x->s, 0, e));
            x->s = bstr_cut(x->s, e + 3);
            continue;
        }
        if (xeat(x, "<?")) {
            if (!xskip_to(x, "?>"))
                return NULL;
            continue;
        }
        struct xnode *kid = xparse_element(x, depth + 1);
        if (!kid)
            return NULL;
        MP_TARRAY_APPEND(n, n->kids, n->num_kids, kid);
    }
}

static struct xnode *xparse(void *ctx, bstr text)
{
    struct xparser x = {.ctx = ctx, .s = text};
    for (;;) {
        xskip_ws(&x);
        if (xeat(&x, "<?")) {
            if (!xskip_to(&x, "?>"))
                return NULL;
        } else if (xeat(&x, "<!--")) {
            if (!xskip_to(&x, "-->"))
                return NULL;
        } else if (xeat(&x, "<!")) {
            if (!xskip_to(&x, ">"))
                return NULL;
        } else {
            break;
        }
    }
    struct xnode *root = xparse_element(&x, 0);
    return x.failed ? NULL : root;
}

static const char *xattr(struct xnode *n, const char *key)
{
    if (!n)
        return NULL;
    for (int i = 0; i < n->num_attr; i++) {
        if (!strcmp(n->attrs[i].key, key))
            return n->attrs[i].val;
    }
    return NULL;
}

static struct xnode *xkid(struct xnode *n, const char *name, int idx)
{
    if (!n)
        return NULL;
    for (int i = 0; i < n->num_kids; i++) {
        if (!strcmp(n->kids[i]->name, name) && idx-- == 0)
            return n->kids[i];
    }
    return NULL;
}

static int xcount(struct xnode *n, const char *name)
{
    int c = 0;
    for (int i = 0; n && i < n->num_kids; i++)
        c += !strcmp(n->kids[i]->name, name);
    return c;
}

static double xnum(struct xnode *n, const char *key, double def)
{
    const char *v = xattr(n, key);
    return v ? atof(v) : def;
}

static double parse_duration(const char *s)
{
    if (!s || *s != 'P')
        return -1;
    s++;
    double total = 0;
    bool time = false;
    while (*s) {
        if (*s == 'T') {
            time = true;
            s++;
            continue;
        }
        char *end;
        double v = strtod(s, &end);
        if (end == s)
            return -1;
        s = end;
        switch (*s) {
        case 'Y': total += v * 365 * 86400; break;
        case 'M': total += time ? v * 60 : v * 30 * 86400; break;
        case 'W': total += v * 7 * 86400; break;
        case 'D': total += v * 86400; break;
        case 'H': total += v * 3600; break;
        case 'S': total += v; break;
        default: return -1;
        }
        s++;
    }
    return total;
}

static double parse_datetime(const char *s)
{
    if (!s)
        return -1;
    struct tm tm = {0};
    int n = 0;
    double sec = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%lf%n", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &sec, &n) < 6)
        return -1;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_sec = (int)sec;
    return (double)timegm(&tm) + (sec - floor(sec));
}

struct dash {
    char *url;
    void *mpd_ctx;
    struct xnode *mpd;
    double fetched;
};

struct rep_front {
    int as;
    char *id;
};

static struct xnode *period_of(struct dash *d)
{
    return xkid(d->mpd, "Period", 0);
}

static struct xnode *find_rep(struct dash *d, struct rep_front *f)
{
    struct xnode *as = xkid(period_of(d), "AdaptationSet", f->as);
    for (int i = 0; as && i < as->num_kids; i++) {
        struct xnode *r = as->kids[i];
        const char *id = xattr(r, "id");
        if (!strcmp(r->name, "Representation") && id && !strcmp(id, f->id))
            return r;
    }
    return NULL;
}

static bool is_dynamic(struct dash *d)
{
    const char *t = xattr(d->mpd, "type");
    return t && !strcmp(t, "dynamic");
}

static char *expand(void *ctx, const char *tmpl, const char *rep_id, int bw,
                    int64_t number, int64_t ts)
{
    char *out = talloc_strdup(ctx, "");
    while (*tmpl) {
        if (*tmpl != '$') {
            out = talloc_strndup_append(out, tmpl, 1);
            tmpl++;
            continue;
        }
        const char *end = strchr(tmpl + 1, '$');
        if (!end)
            break;
        char *id = talloc_strndup(ctx, tmpl + 1, end - tmpl - 1);
        tmpl = end + 1;
        if (!*id) {
            out = talloc_strdup_append(out, "$");
            continue;
        }
        int width = 0;
        char *pct = strchr(id, '%');
        if (pct) {
            *pct = 0;
            width = atoi(pct + 2);
        }
        long long v = 0;
        if (!strcmp(id, "RepresentationID")) {
            out = talloc_strdup_append(out, rep_id);
            continue;
        } else if (!strcmp(id, "Number")) {
            v = number;
        } else if (!strcmp(id, "Time")) {
            v = ts;
        } else if (!strcmp(id, "Bandwidth")) {
            v = bw;
        } else {
            continue;
        }
        out = talloc_asprintf_append(out, "%0*lld", width, v);
    }
    return out;
}

static char *base_for(struct dash *d, void *ctx, struct xnode *as,
                      struct xnode *rep)
{
    char *base = talloc_strdup(ctx, d->url);
    struct xnode *chain[4] = {d->mpd, period_of(d), as, rep};
    for (int i = 0; i < 4; i++) {
        struct xnode *b = xkid(chain[i], "BaseURL", 0);
        if (b && b->text)
            base = ad_resolve_url(ctx, base, b->text);
    }
    return base;
}

static const char *tpl_attr(struct xnode *rep, struct xnode *as,
                            struct xnode *per, const char *key)
{
    struct xnode *c[3] = {xkid(rep, "SegmentTemplate", 0),
                          xkid(as, "SegmentTemplate", 0),
                          xkid(per, "SegmentTemplate", 0)};
    for (int i = 0; i < 3; i++) {
        const char *v = xattr(c[i], key);
        if (v)
            return v;
    }
    return NULL;
}

static double tpl_num(struct xnode *rep, struct xnode *as, struct xnode *per,
                      const char *key, double def)
{
    const char *v = tpl_attr(rep, as, per, key);
    return v ? atof(v) : def;
}

static struct xnode *tpl_timeline(struct xnode *rep, struct xnode *as,
                                  struct xnode *per)
{
    struct xnode *c[3] = {xkid(rep, "SegmentTemplate", 0),
                          xkid(as, "SegmentTemplate", 0),
                          xkid(per, "SegmentTemplate", 0)};
    for (int i = 0; i < 3; i++) {
        struct xnode *t = xkid(c[i], "SegmentTimeline", 0);
        if (t)
            return t;
    }
    return NULL;
}

static void add_seg(struct playlist *pl, char *url, char *map, double dur,
                    double start, int64_t seq)
{
    struct seg s = {
        .url = url,
        .map_url = map,
        .dur = dur,
        .start = start,
        .seq = seq,
    };
    MP_TARRAY_APPEND(pl, pl->segs, pl->num, s);
    pl->total = start + dur;
}

static void refresh_mpd(struct demuxer *demuxer, struct dash *d)
{
    double min_update = parse_duration(xattr(d->mpd, "minimumUpdatePeriod"));
    if (min_update < 1)
        min_update = 1;
    double now = mp_time_sec();
    if (!is_dynamic(d) || now - d->fetched < min_update)
        return;
    d->fetched = now;
    void *ctx = talloc_new(NULL);
    bstr text = ad_fetch(demuxer, ctx, d->url, 8 * 1024 * 1024);
    void *mctx = talloc_new(NULL);
    struct xnode *mpd = text.len ? xparse(mctx, text) : NULL;
    if (!mpd || strcmp(mpd->name, "MPD") || !xkid(mpd, "Period", 0)) {
        talloc_free(mctx);
    } else {
        talloc_free(d->mpd_ctx);
        d->mpd_ctx = mctx;
        d->mpd = mpd;
    }
    talloc_free(ctx);
}

static struct playlist *dash_load(struct demuxer *demuxer, struct variant *v)
{
    struct dash *d = ad_front(demuxer);
    struct rep_front *f = v->front;
    if (v->pl)
        refresh_mpd(demuxer, d);

    struct xnode *per = period_of(d);
    struct xnode *as = xkid(per, "AdaptationSet", f->as);
    struct xnode *rep = find_rep(d, f);
    if (!rep)
        return NULL;

    void *own = talloc_new(NULL);
    struct playlist *pl = talloc_zero(own, struct playlist);
    pl->own = own;
    char *base = base_for(d, own, as, rep);
    int bw = (int)xnum(rep, "bandwidth", 0);
    const char *id = f->id;
    bool dynamic = is_dynamic(d);

    double pstart = parse_duration(xattr(per, "start"));
    if (pstart < 0)
        pstart = 0;
    double pdur = parse_duration(xattr(per, "duration"));
    if (pdur < 0) {
        double total = parse_duration(xattr(d->mpd, "mediaPresentationDuration"));
        pdur = total > 0 ? total - pstart : -1;
    }

    const char *media = tpl_attr(rep, as, per, "media");
    const char *init = tpl_attr(rep, as, per, "initialization");
    if (media) {
        double ts = tpl_num(rep, as, per, "timescale", 1);
        if (ts <= 0)
            ts = 1;
        int64_t start_num = (int64_t)tpl_num(rep, as, per, "startNumber", 1);
        double pto = tpl_num(rep, as, per, "presentationTimeOffset", 0);
        char *map = init ? ad_resolve_url(own, base, expand(own, init, id, bw, 0, 0))
                         : NULL;
        struct xnode *tl = tpl_timeline(rep, as, per);
        pl->first_seq = start_num;
        if (tl) {
            int64_t t = 0, number = start_num;
            double dur_max = 0;
            for (int i = 0; i < tl->num_kids; i++) {
                struct xnode *s = tl->kids[i];
                if (strcmp(s->name, "S"))
                    continue;
                if (xattr(s, "t"))
                    t = atoll(xattr(s, "t"));
                int64_t dd = (int64_t)xnum(s, "d", 0);
                int64_t r = (int64_t)xnum(s, "r", 0);
                if (dd <= 0)
                    continue;
                if (r < 0) {
                    double limit = pdur > 0 ? (pstart + pdur) * ts : 1e18;
                    r = 0;
                    while (r < 100000 && t + (r + 1) * dd <= limit - pstart * ts + pto)
                        r++;
                    r = MPMAX(r - 1, 0);
                }
                for (int64_t k = 0; k <= r; k++) {
                    char *u = ad_resolve_url(own, base,
                                             expand(own, media, id, bw, number, t));
                    add_seg(pl, u, map, dd / ts, (t - pto) / ts, number);
                    dur_max = MPMAX(dur_max, dd / ts);
                    t += dd;
                    number++;
                }
            }
            pl->target = dur_max;
            if (dynamic && pl->num) {
                double first = pl->segs[0].start;
                for (int i = 0; i < pl->num; i++)
                    pl->segs[i].start -= first;
                pl->total -= first;
            }
        } else {
            double dd = tpl_num(rep, as, per, "duration", 0);
            if (dd <= 0) {
                talloc_free(own);
                return NULL;
            }
            double sdur = dd / ts;
            pl->target = sdur;
            int64_t first = 0, last;
            if (dynamic) {
                double ast = parse_datetime(xattr(d->mpd, "availabilityStartTime"));
                double now = (double)time(NULL);
                double edge = (now - ast - pstart) / sdur;
                last = (int64_t)floor(edge) - 1;
                double tsbd = parse_duration(xattr(d->mpd, "timeShiftBufferDepth"));
                int64_t win = tsbd > 0 ? (int64_t)(tsbd / sdur) : 10;
                win = MPMAX(MPMIN(win, 30), 3);
                first = MPMAX(last - win + 1, 0);
                if (last < first)
                    last = first;
            } else {
                last = pdur > 0 ? (int64_t)ceil(pdur / sdur) - 1 : 0;
            }
            pl->first_seq = start_num + first;
            for (int64_t n = first; n <= last; n++) {
                int64_t number = start_num + n;
                int64_t t = (int64_t)(n * dd) + (int64_t)pto;
                char *u = ad_resolve_url(own, base,
                                         expand(own, media, id, bw, number, t));
                add_seg(pl, u, map, sdur, (n - first) * sdur, number);
            }
        }
    } else {
        struct xnode *list = xkid(rep, "SegmentList", 0);
        if (!list)
            list = xkid(as, "SegmentList", 0);
        if (!list)
            list = xkid(per, "SegmentList", 0);
        if (!list) {
            talloc_free(own);
            return NULL;
        }
        double ts = xnum(list, "timescale", 1);
        double dd = xnum(list, "duration", 0) / ts;
        struct xnode *in = xkid(list, "Initialization", 0);
        char *map = in && xattr(in, "sourceURL")
                  ? ad_resolve_url(own, base, xattr(in, "sourceURL")) : NULL;
        pl->first_seq = (int64_t)xnum(list, "startNumber", 1);
        for (int i = 0; i < list->num_kids; i++) {
            struct xnode *u = list->kids[i];
            if (strcmp(u->name, "SegmentURL") || !xattr(u, "media") ||
                xattr(u, "mediaRange"))
                continue;
            add_seg(pl, ad_resolve_url(own, base, xattr(u, "media")), map, dd,
                    pl->total, pl->first_seq + pl->num);
        }
        pl->target = dd;
    }

    if (!pl->num) {
        talloc_free(own);
        return NULL;
    }
    pl->endlist = !dynamic;
    if (pl->target <= 0)
        pl->target = pl->total / pl->num;
    return pl;
}

static const struct ad_ops dash_ops = {
    .load = dash_load,
};

static const char *content_type(struct xnode *as)
{
    const char *ct = xattr(as, "contentType");
    const char *mt = xattr(as, "mimeType");
    if (!mt && as->num_kids)
        mt = xattr(xkid(as, "Representation", 0), "mimeType");
    if (ct)
        return ct;
    if (!mt)
        return "";
    if (!strncmp(mt, "video/", 6))
        return "video";
    if (!strncmp(mt, "audio/", 6))
        return "audio";
    if (!strncmp(mt, "image/", 6))
        return "image";
    return "text";
}

static int cmp_bw(const void *a, const void *b)
{
    const struct variant *x = *(struct variant *const *)a;
    const struct variant *y = *(struct variant *const *)b;
    return x->bw - y->bw;
}

static bool rep_has_segments(struct xnode *rep, struct xnode *as,
                             struct xnode *per)
{
    return tpl_attr(rep, as, per, "media") || xkid(rep, "SegmentList", 0) ||
           xkid(as, "SegmentList", 0) || xkid(per, "SegmentList", 0);
}

static int d_open(struct demuxer *demuxer, enum demux_check check)
{
    struct MPOpts *o = mp_get_config_group(NULL, demuxer->global, &mp_opt_root);
    bool enabled = o->dash_native;
    talloc_free(o);
    if (!enabled || !demuxer->stream || demuxer->depth > 0)
        return -1;

    char probe[4096];
    int len = stream_read_peek(demuxer->stream, probe, sizeof(probe));
    bstr head = {probe, len};
    if (bstr_find0(head, "<MPD") < 0)
        return -1;

    void *ctx = talloc_new(demuxer);
    struct dash *d = talloc_zero(ctx, struct dash);
    d->url = talloc_strdup(d, demuxer->stream->url);
    d->fetched = mp_time_sec();
    bstr text = stream_read_complete(demuxer->stream, ctx, 8 * 1024 * 1024);
    d->mpd_ctx = talloc_new(NULL);
    d->mpd = text.len ? xparse(d->mpd_ctx, text) : NULL;
    if (!d->mpd || strcmp(d->mpd->name, "MPD"))
        return -1;
    if (xcount(d->mpd, "Period") != 1)
        return -1;

    struct xnode *per = period_of(d);
    struct ad_track *tracks = NULL;
    int num_tracks = 0;
    bool have_video = false;

    for (int a = 0; a < xcount(per, "AdaptationSet"); a++) {
        struct xnode *as = xkid(per, "AdaptationSet", a);
        const char *type = content_type(as);
        if (xkid(as, "ContentProtection", 0))
            return -1;
        if (!strcmp(type, "text")) {
            MP_VERBOSE(demuxer, "Subtitles in manifest, leaving it to FFmpeg.\n");
            return -1;
        }
        bool video = !strcmp(type, "video");
        if ((!video && strcmp(type, "audio")) || (video && have_video))
            continue;

        struct variant **vars = NULL;
        int num_vars = 0;
        for (int r = 0; r < xcount(as, "Representation"); r++) {
            struct xnode *rep = xkid(as, "Representation", r);
            if (xkid(rep, "ContentProtection", 0))
                return -1;
            if (!rep_has_segments(rep, as, per)) {
                MP_VERBOSE(demuxer, "Representation without segment list.\n");
                return -1;
            }
            struct variant *v = talloc_zero(ctx, struct variant);
            struct rep_front *f = talloc_zero(v, struct rep_front);
            f->as = a;
            f->id = talloc_strdup(f, (xattr(rep, "id") ? xattr(rep, "id") : ""));
            v->front = f;
            v->url = f->id;
            v->bw = (int)xnum(rep, "bandwidth", 0);
            v->w = (int)xnum(rep, "width", xnum(as, "width", 0));
            v->h = (int)xnum(rep, "height", xnum(as, "height", 0));
            v->video = video;
            MP_TARRAY_APPEND(ctx, vars, num_vars, v);
        }
        if (!num_vars)
            continue;
        qsort(vars, num_vars, sizeof(vars[0]), cmp_bw);

        struct ad_track t = {
            .vars = vars,
            .num_vars = num_vars,
            .main = video,
            .indep = !video,
            .lang = (char *)xattr(as, "lang"),
            .title = (char *)xattr(as, "label"),
        };
        t.cur = ad_initial_variant(demuxer, vars, num_vars);
        if (t.cur < 0)
            t.cur = num_vars - 1;
        if (!video) {
            for (int n = 0; n < num_vars; n++) {
                if (vars[n]->bw <= 128000)
                    t.cur = n;
            }
        }
        if (video) {
            have_video = true;
            MP_TARRAY_INSERT_AT(ctx, tracks, num_tracks, 0, t);
        } else {
            t.def = num_tracks == (have_video ? 1 : 0);
            MP_TARRAY_APPEND(ctx, tracks, num_tracks, t);
        }
    }
    if (!num_tracks)
        return -1;
    if (!tracks[0].main)
        return -1;

    return ad_open(demuxer, &dash_ops, d, tracks, num_tracks, "dash");
}

const demuxer_desc_t demuxer_desc_dash = {
    .name = "dash",
    .desc = "DASH manifest",
    .open = d_open,
    .read_packet = ad_read_packet,
    .close = ad_close,
    .seek = ad_seek,
    .switched_tracks = ad_switched_tracks,
};
