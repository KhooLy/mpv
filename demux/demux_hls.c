#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include "common/common.h"
#include "common/msg.h"
#include "misc/bstr.h"
#include "options/m_config.h"
#include "options/options.h"
#include "osdep/timer.h"
#include "stream/stream.h"

#include "demux.h"
#include "packet.h"
#include "stheader.h"

struct seg {
    char *url;
    char *map_url;
    double dur;
    double start;
    int64_t seq;
    bool discont;
};

struct playlist {
    void *own;
    struct seg *segs;
    int num;
    int64_t first_seq;
    double target;
    double total;
    bool endlist;
};

struct variant {
    char *url;
    char *audio;
    int bw, w, h;
    bool video;
    bool bad;
    struct playlist *pl;
};

struct media {
    char *group, *name, *lang, *url;
    bool def;
};

struct master {
    struct variant **vars;
    int num_vars;
    struct media *media;
    int num_media;
    bool has_subs;
};

struct vstream {
    struct sh_stream *sh;
    bool selected;
};

struct lane {
    bool main;
    struct variant **vars;
    int num_vars;
    int cur;
    int fails;
    struct mp_codec_params ***codecs;

    struct vstream **vs;
    int num_vs;

    struct demuxer *d;
    struct vstream **map;
    int num_map;
    struct demuxer **anchors;
    int num_anchors;

    int64_t seq;
    bool opened_any;
    bool pending_discont;
    double ts_offset;
    double end_ts;
    double dts;
    double base;
    bool have_base;
    bool any_selected;
    bool eof;
    struct demux_packet *next;

    int64_t seg_bytes;
    int64_t seg_ns;
    int up_count;
    double last_switch;

    char *init_url;
    bstr init;
};

struct priv {
    char *base_url;
    struct master master;
    struct variant *single;
    struct lane **lanes;
    int num_lanes;
    double duration;
    bool live;
    double rate_fast, rate_slow;
};

static char *attr_get(void *ctx, bstr list, const char *key)
{
    bstr k = bstr0(key);
    while (list.len) {
        list = bstr_lstrip(list);
        while (list.len && list.start[0] == ',')
            list = bstr_cut(list, 1);
        int eq = bstrchr(list, '=');
        if (eq < 0)
            return NULL;
        bstr name = bstr_strip(bstr_splice(list, 0, eq));
        list = bstr_cut(list, eq + 1);
        bstr val;
        if (list.len && list.start[0] == '"') {
            list = bstr_cut(list, 1);
            int q = bstrchr(list, '"');
            if (q < 0)
                q = list.len;
            val = bstr_splice(list, 0, q);
            list = bstr_cut(list, q + 1);
        } else {
            int c = bstrchr(list, ',');
            if (c < 0)
                c = list.len;
            val = bstr_splice(list, 0, c);
            list = bstr_cut(list, c);
        }
        if (bstrcasecmp(name, k) == 0)
            return bstrto0(ctx, val);
    }
    return NULL;
}

static char *resolve_url(void *ctx, const char *base, const char *ref)
{
    if (strstr(ref, "://"))
        return talloc_strdup(ctx, ref);
    const char *scheme_end = strstr(base, "://");
    if (!scheme_end)
        scheme_end = base - 3;
    const char *host = scheme_end + 3;
    if (ref[0] == '/' && ref[1] == '/')
        return talloc_asprintf(ctx, "%.*s%s", (int)(host - base) - 2, base, ref);
    size_t blen = strcspn(base, "?#");
    if (ref[0] == '/') {
        const char *path = strchr(host, '/');
        size_t hlen = path && (size_t)(path - base) < blen ? path - base : blen;
        return talloc_asprintf(ctx, "%.*s%s", (int)hlen, base, ref);
    }
    const char *slash = NULL;
    for (const char *c = host; c < base + blen; c++) {
        if (*c == '/')
            slash = c;
    }
    size_t dlen = slash ? slash - base + 1 : blen;
    if (!slash)
        return talloc_asprintf(ctx, "%.*s/%s", (int)dlen, base, ref);
    return talloc_asprintf(ctx, "%.*s%s", (int)dlen, base, ref);
}

static bool has_video_codec(const char *codecs)
{
    if (!codecs)
        return false;
    static const char *const v[] = {"avc", "hvc", "hev", "av01", "vp0", "vp9",
                                    "dvh", "dva", "dvav", NULL};
    for (int n = 0; v[n]; n++) {
        if (strstr(codecs, v[n]))
            return true;
    }
    return false;
}

static bool parse_playlist(void *ctx, struct demuxer *log, bstr text,
                           const char *url, struct playlist **out_pl,
                           struct master *master)
{
    struct playlist *pl = talloc_zero(ctx, struct playlist);
    pl->own = ctx;
    bool is_master = false;
    double next_dur = -1;
    bool next_discont = false;
    char *map_url = NULL;
    char *pending_stream_inf = NULL;

    while (text.len) {
        bstr line = bstr_strip(bstr_getline(text, &text));
        if (!line.len)
            continue;
        if (line.start[0] != '#') {
            char *abs = resolve_url(ctx, url, bstrto0(ctx, line));
            if (pending_stream_inf) {
                struct variant *v = talloc_zero(ctx, struct variant);
                v->url = abs;
                char *bw = attr_get(ctx, bstr0(pending_stream_inf), "BANDWIDTH");
                char *abw = attr_get(ctx, bstr0(pending_stream_inf), "AVERAGE-BANDWIDTH");
                v->bw = bw ? atoi(bw) : abw ? atoi(abw) : 0;
                char *res = attr_get(ctx, bstr0(pending_stream_inf), "RESOLUTION");
                if (res)
                    sscanf(res, "%dx%d", &v->w, &v->h);
                char *codecs = attr_get(ctx, bstr0(pending_stream_inf), "CODECS");
                v->video = v->h > 0 || has_video_codec(codecs);
                v->audio = attr_get(ctx, bstr0(pending_stream_inf), "AUDIO");
                MP_TARRAY_APPEND(ctx, master->vars, master->num_vars, v);
                pending_stream_inf = NULL;
                continue;
            }
            struct seg s = {
                .url = abs,
                .map_url = map_url,
                .dur = next_dur > 0 ? next_dur : 0,
                .discont = next_discont,
                .seq = pl->first_seq + pl->num,
                .start = pl->total,
            };
            MP_TARRAY_APPEND(pl, pl->segs, pl->num, s);
            pl->total += s.dur;
            next_dur = -1;
            next_discont = false;
            continue;
        }

        bstr tag = line;
        if (bstr_eatstart0(&tag, "#EXTINF:")) {
            next_dur = strtod(bstrto0(ctx, tag), NULL);
        } else if (bstr_eatstart0(&tag, "#EXT-X-TARGETDURATION:")) {
            pl->target = atof(bstrto0(ctx, tag));
        } else if (bstr_eatstart0(&tag, "#EXT-X-MEDIA-SEQUENCE:")) {
            pl->first_seq = atoll(bstrto0(ctx, tag));
        } else if (bstr_equals0(tag, "#EXT-X-DISCONTINUITY")) {
            next_discont = true;
        } else if (bstr_equals0(tag, "#EXT-X-ENDLIST")) {
            pl->endlist = true;
        } else if (bstr_eatstart0(&tag, "#EXT-X-MAP:")) {
            if (attr_get(ctx, tag, "BYTERANGE"))
                goto unsupported;
            char *u = attr_get(ctx, tag, "URI");
            map_url = u ? resolve_url(ctx, url, u) : NULL;
        } else if (bstr_eatstart0(&tag, "#EXT-X-KEY:")) {
            char *m = attr_get(ctx, tag, "METHOD");
            if (m && strcmp(m, "NONE"))
                goto unsupported;
        } else if (bstr_startswith0(line, "#EXT-X-BYTERANGE") ||
                   bstr_startswith0(line, "#EXT-X-I-FRAMES-ONLY") ||
                   bstr_startswith0(line, "#EXT-X-DEFINE"))
        {
            goto unsupported;
        } else if (bstr_eatstart0(&tag, "#EXT-X-STREAM-INF:")) {
            is_master = true;
            pending_stream_inf = bstrto0(ctx, tag);
        } else if (bstr_eatstart0(&tag, "#EXT-X-MEDIA:")) {
            is_master = true;
            char *type = attr_get(ctx, tag, "TYPE");
            if (!type)
                continue;
            if (!strcmp(type, "SUBTITLES")) {
                master->has_subs = true;
                continue;
            }
            if (strcmp(type, "AUDIO"))
                continue;
            struct media m = {
                .group = attr_get(ctx, tag, "GROUP-ID"),
                .name = attr_get(ctx, tag, "NAME"),
                .lang = attr_get(ctx, tag, "LANGUAGE"),
            };
            char *u = attr_get(ctx, tag, "URI");
            m.url = u ? resolve_url(ctx, url, u) : NULL;
            char *def = attr_get(ctx, tag, "DEFAULT");
            m.def = def && !strcmp(def, "YES");
            MP_TARRAY_APPEND(ctx, master->media, master->num_media, m);
        }
    }

    if (is_master) {
        if (!master->num_vars)
            return false;
        *out_pl = NULL;
        return true;
    }
    if (!pl->num)
        return false;
    if (pl->target <= 0)
        pl->target = pl->total / pl->num;
    *out_pl = pl;
    return true;

unsupported:
    MP_VERBOSE(log, "Unsupported HLS feature, not using native demuxer.\n");
    return false;
}

static bstr fetch(struct demuxer *demuxer, void *ctx, const char *url, int max)
{
    struct stream *s = stream_create(url, STREAM_READ | demuxer->stream_origin,
                                     demuxer->cancel, demuxer->global);
    if (!s)
        return (bstr){0};
    bstr res = stream_read_complete(s, ctx, max);
    free_stream(s);
    return res;
}

static struct playlist *load_playlist(struct demuxer *demuxer, const char *url)
{
    void *tmp = talloc_new(NULL);
    void *own = talloc_new(NULL);
    bstr text = fetch(demuxer, tmp, url, 8 * 1024 * 1024);
    struct playlist *pl = NULL;
    struct master m = {0};
    if (!text.len || !parse_playlist(own, demuxer, text, url, &pl, &m) || !pl)
    {
        talloc_free(own);
        pl = NULL;
    }
    talloc_free(tmp);
    return pl;
}

static bool codec_same(struct mp_codec_params *a, struct mp_codec_params *b)
{
    if (a->type != b->type)
        return false;
    const char *ca = a->codec, *cb = b->codec;
    if (!ca || !cb || strcmp(ca, cb))
        return false;
    if (a->extradata_size != b->extradata_size ||
        (a->extradata_size && memcmp(a->extradata, b->extradata,
                                     a->extradata_size)))
        return false;
    if (a->type == STREAM_AUDIO)
        return a->samplerate == b->samplerate &&
               mp_chmap_equals(&a->channels, &b->channels);
    return true;
}

static struct variant *lane_var(struct lane *l)
{
    return l->vars[l->cur];
}

static bool vs_selected(struct lane *l)
{
    for (int n = 0; n < l->num_vs; n++) {
        if (l->vs[n]->selected)
            return true;
    }
    return false;
}

static void apply_selection(struct lane *l)
{
    if (!l->d)
        return;
    for (int n = 0; n < l->num_map; n++) {
        bool sel = l->map[n] && l->map[n]->selected;
        demuxer_select_track(l->d, demux_get_stream(l->d, n), MP_NOPTS_VALUE,
                             sel);
    }
}

static bool bind_streams(struct demuxer *demuxer, struct lane *l,
                         struct demuxer *d, bool create)
{
    int n_sub = demux_get_num_stream(d);
    struct vstream **map = talloc_zero_array(l, struct vstream *, n_sub);
    bool used[64] = {0};
    for (int i = 0; i < n_sub; i++) {
        struct sh_stream *sh = demux_get_stream(d, i);
        if (sh->type != STREAM_VIDEO && sh->type != STREAM_AUDIO)
            continue;
        if (create) {
            if ((!l->main && l->num_vs) || l->num_vs >= 16)
                continue;
            if (!l->main && sh->type != STREAM_AUDIO)
                continue;
            struct sh_stream *new = demux_alloc_sh_stream(sh->type);
            new->codec = sh->codec;
            new->lang = sh->lang;
            new->title = sh->title;
            new->default_track = sh->default_track;
            demux_add_sh_stream(demuxer, new);
            struct vstream *vs = talloc_zero(l, struct vstream);
            vs->sh = new;
            MP_TARRAY_APPEND(l, l->vs, l->num_vs, vs);
        }
        for (int j = 0; j < l->num_vs && j < 64; j++) {
            if (!used[j] && l->vs[j]->sh->type == sh->type) {
                used[j] = true;
                map[i] = l->vs[j];
                break;
            }
        }
    }
    for (int j = 0; j < l->num_vs && j < 64; j++) {
        if (!used[j]) {
            talloc_free(map);
            return false;
        }
    }
    talloc_free(l->map);
    l->map = map;
    l->num_map = n_sub;
    return true;
}

static void note_codecs(struct lane *l, struct demuxer *d)
{
    struct mp_codec_params **c = l->codecs[l->cur];
    if (c[0])
        return;
    bool anchor = false;
    for (int i = 0; i < l->num_map; i++) {
        struct vstream *vs = l->map[i];
        if (!vs)
            continue;
        int slot = -1;
        for (int j = 0; j < l->num_vs; j++) {
            if (l->vs[j] == vs)
                slot = j;
        }
        struct mp_codec_params *mine = demux_get_stream(d, i)->codec;
        if (codec_same(vs->sh->codec, mine)) {
            c[slot] = vs->sh->codec;
        } else {
            c[slot] = mine;
            anchor = true;
        }
    }
    if (anchor || l->num_anchors == 0)
        MP_TARRAY_APPEND(l, l->anchors, l->num_anchors, d);
}

static bool is_anchor(struct lane *l, struct demuxer *d)
{
    for (int n = 0; n < l->num_anchors; n++) {
        if (l->anchors[n] == d)
            return true;
    }
    return false;
}

static void drop_current(struct lane *l)
{
    if (l->d && !is_anchor(l, l->d))
        demux_free(l->d);
    l->d = NULL;
}

static struct seg *find_seg(struct playlist *pl, int64_t seq)
{
    int64_t i = seq - pl->first_seq;
    return i >= 0 && i < pl->num ? &pl->segs[i] : NULL;
}

static bool ensure_playlist(struct demuxer *demuxer, struct variant *v)
{
    if (v->pl)
        return true;
    v->pl = load_playlist(demuxer, v->url);
    if (!v->pl)
        v->bad = true;
    return v->pl;
}

static bool reload_live(struct demuxer *demuxer, struct lane *l)
{
    struct variant *v = lane_var(l);
    struct playlist *pl = load_playlist(demuxer, v->url);
    if (!pl)
        return false;
    talloc_free(v->pl->own);
    v->pl = pl;
    return true;
}

static struct demuxer *open_segment(struct demuxer *demuxer, struct lane *l,
                                    struct seg *s)
{
    if (s->map_url) {
        if (!l->init_url || strcmp(l->init_url, s->map_url)) {
            talloc_free(l->init.start);
            l->init = fetch(demuxer, l, s->map_url, 2 * 1024 * 1024);
            talloc_free(l->init_url);
            l->init_url = talloc_strdup(l, s->map_url);
            if (!l->init.len)
                return NULL;
        }
    } else {
        l->init = (bstr){0};
    }
    struct demuxer_params params = {
        .init_fragment = s->map_url ? l->init : (bstr){0},
        .stream_flags = demuxer->stream_origin,
        .depth = demuxer->depth + 1,
    };
    return demux_open_url(s->url, &params, demuxer->cancel, demuxer->global);
}

static void account(struct lane *l, struct demuxer *d, int64_t t0)
{
    l->seg_ns += mp_time_ns() - t0;
    l->seg_bytes += demux_get_bytes_read_hack(d);
}

static bool start_segment(struct demuxer *demuxer, struct lane *l)
{
    struct priv *p = demuxer->priv;
    struct variant *v = lane_var(l);
    if (!ensure_playlist(demuxer, v))
        return false;
    struct seg *s = find_seg(v->pl, l->seq);
    if (!s)
        return false;

    int64_t t0 = mp_time_ns();
    struct demuxer *d = open_segment(demuxer, l, s);
    if (!d) {
        if (!demux_cancel_test(demuxer))
            MP_ERR(demuxer, "Failed to open segment %s\n", s->url);
        return false;
    }
    l->d = d;
    if (!bind_streams(demuxer, l, d, false)) {
        MP_WARN(demuxer, "Variant %d has an incompatible layout.\n", l->cur);
        v->bad = true;
        drop_current(l);
        return false;
    }
    note_codecs(l, d);
    l->seg_ns = 0;
    l->seg_bytes = 0;
    account(l, d, t0);

    if (l->opened_any && (s->discont || l->pending_discont)) {
        l->ts_offset = l->end_ts - d->start_time;
        MP_VERBOSE(demuxer, "Discontinuity, new offset %f\n", l->ts_offset);
    }
    l->pending_discont = false;
    l->opened_any = true;
    if (!l->have_base) {
        l->base = d->start_time - s->start;
        l->have_base = true;
    }
    demux_set_ts_offset(d, l->ts_offset);
    apply_selection(l);
    (void)p;
    return true;
}

static void update_rates(struct priv *p, struct lane *l)
{
    if (l->seg_bytes < 32768 || l->seg_ns < 5 * INT64_C(1000000))
        return;
    double bps = l->seg_bytes * 8.0 * 1e9 / l->seg_ns;
    p->rate_fast = p->rate_fast > 0 ? 0.5 * p->rate_fast + 0.5 * bps : bps;
    p->rate_slow = p->rate_slow > 0 ? 0.9 * p->rate_slow + 0.1 * bps : bps;
}

static bool var_allowed(struct demuxer *demuxer, struct variant *v)
{
    struct MPOpts *o = mp_get_config_group(NULL, demuxer->global, &mp_opt_root);
    bool ok = (!o->abr_max_bitrate || v->bw <= o->abr_max_bitrate) &&
              (!o->abr_max_height || !v->h || v->h <= o->abr_max_height);
    talloc_free(o);
    return ok;
}

static int pick_next_variant(struct demuxer *demuxer, struct lane *l)
{
    struct priv *p = demuxer->priv;
    struct MPOpts *o = mp_get_config_group(NULL, demuxer->global, &mp_opt_root);
    bool adaptive = o->hls_adaptive;
    talloc_free(o);

    struct variant *cur = lane_var(l);
    if (!adaptive || p->rate_fast <= 0 || l->num_vars < 2)
        return l->cur;

    double budget = MPMIN(p->rate_fast, p->rate_slow) * 0.75;
    int best = -1, lowest = -1;
    for (int n = 0; n < l->num_vars; n++) {
        struct variant *v = l->vars[n];
        if (v->bad || v->video != cur->video || !var_allowed(demuxer, v))
            continue;
        if (lowest < 0 || v->bw < l->vars[lowest]->bw)
            lowest = n;
        if (v->bw <= budget && (best < 0 || v->bw > l->vars[best]->bw))
            best = n;
    }
    if (best < 0)
        best = lowest;
    if (best < 0 || best == l->cur)
        return l->cur;

    double now = mp_time_sec();
    if (l->vars[best]->bw > cur->bw) {
        l->up_count++;
        if (l->up_count < 2 || now - l->last_switch < 8)
            return l->cur;
    }
    l->up_count = 0;
    l->last_switch = now;
    MP_VERBOSE(demuxer, "Adaptive: %d -> %d kbit/s (estimate %d kbit/s)\n",
               cur->bw / 1000, l->vars[best]->bw / 1000, (int)(budget / 0.75 / 1000));
    return best;
}

static void follow_main(struct priv *p, struct lane *l, struct lane *main)
{
    if (l->main || l->num_vars < 2)
        return;
    const char *group = lane_var(main)->audio;
    for (int n = 0; n < l->num_vars; n++) {
        struct variant *v = l->vars[n];
        if (group && v->audio && !strcmp(group, v->audio) && !v->bad) {
            l->cur = n;
            return;
        }
    }
}

static void finish_segment(struct demuxer *demuxer, struct lane *l)
{
    struct priv *p = demuxer->priv;
    update_rates(p, l);
    drop_current(l);
    l->seq++;
    if (l->main) {
        l->cur = pick_next_variant(demuxer, l);
    } else {
        follow_main(p, l, p->lanes[0]);
    }
}

static bool wait_for_segment(struct demuxer *demuxer, struct lane *l)
{
    struct variant *v = lane_var(l);
    double wait = MPMAX(v->pl->target / 2, 0.5);
    int64_t until = mp_time_ns() + (int64_t)(wait * 1e9);
    while (mp_time_ns() < until) {
        if (demux_cancel_test(demuxer))
            return false;
        mp_sleep_ns(50 * INT64_C(1000000));
    }
    if (!reload_live(demuxer, l))
        return true;
    v = lane_var(l);
    if (l->seq < v->pl->first_seq) {
        MP_WARN(demuxer, "Fell behind the live window, skipping ahead.\n");
        l->seq = v->pl->first_seq;
        l->pending_discont = true;
    }
    return true;
}

static void read_lane(struct demuxer *demuxer, struct lane *l)
{
    struct priv *p = demuxer->priv;
    if (l->next)
        return;

    if (!l->d) {
        struct variant *v = lane_var(l);
        if (!ensure_playlist(demuxer, v)) {
            l->eof = true;
            return;
        }
        if (l->seq >= v->pl->first_seq + v->pl->num) {
            if (v->pl->endlist || !p->live) {
                l->eof = true;
                return;
            }
            if (!wait_for_segment(demuxer, l))
                l->eof = true;
            return;
        }
        if (l->seq < v->pl->first_seq) {
            l->seq = v->pl->first_seq;
            l->pending_discont = true;
        }
        if (start_segment(demuxer, l)) {
            l->fails = 0;
            return;
        }
        {
            if (demux_cancel_test(demuxer) || ++l->fails > 3) {
                l->eof = true;
                return;
            }
            int fallback = -1;
            for (int n = 0; n < l->num_vars; n++) {
                if (!l->vars[n]->bad && l->vars[n]->video == v->video) {
                    fallback = n;
                    break;
                }
            }
            if (fallback < 0)
                l->eof = true;
            else
                l->cur = fallback;
        }
        return;
    }

    int64_t t0 = mp_time_ns();
    struct demux_packet *pkt = demux_read_any_packet(l->d);
    account(l, l->d, t0);
    if (!pkt) {
        finish_segment(demuxer, l);
        return;
    }
    if (pkt->stream < 0 || pkt->stream >= l->num_map || !l->map[pkt->stream]) {
        talloc_free(pkt);
        return;
    }
    struct vstream *vs = l->map[pkt->stream];
    int slot = 0;
    for (int j = 0; j < l->num_vs; j++) {
        if (l->vs[j] == vs)
            slot = j;
    }
    struct mp_codec_params *codec = l->codecs[l->cur][slot];
    pkt->segmented = true;
    pkt->codec = codec ? codec : vs->sh->codec;
    pkt->start = pkt->end = MP_NOPTS_VALUE;

    double dts = pkt->dts != MP_NOPTS_VALUE ? pkt->dts : pkt->pts;
    if (l->dts == MP_NOPTS_VALUE || (dts != MP_NOPTS_VALUE && dts > l->dts))
        l->dts = dts;
    if (pkt->pts != MP_NOPTS_VALUE) {
        double end = pkt->pts + MPMAX(pkt->duration, 0);
        l->end_ts = MPMAX(l->end_ts, end);
    }
    pkt->stream = vs->sh->index;
    l->next = pkt;
}

static bool d_read_packet(struct demuxer *demuxer, struct demux_packet **out)
{
    struct priv *p = demuxer->priv;
    struct lane *best = NULL;
    for (int n = 0; n < p->num_lanes; n++) {
        struct lane *l = p->lanes[n];
        if (!l->any_selected || l->eof)
            continue;
        if (!best || l->dts == MP_NOPTS_VALUE ||
            (best->dts != MP_NOPTS_VALUE && l->dts < best->dts))
            best = l;
    }
    if (!best)
        return false;
    read_lane(demuxer, best);
    *out = best->next;
    best->next = NULL;
    return true;
}

static void reselect(struct demuxer *demuxer)
{
    struct priv *p = demuxer->priv;
    for (int n = 0; n < p->num_lanes; n++) {
        struct lane *l = p->lanes[n];
        for (int i = 0; i < l->num_vs; i++)
            l->vs[i]->selected = demux_stream_is_selected(l->vs[i]->sh);
        bool was = l->any_selected;
        l->any_selected = vs_selected(l);
        if (!was && l->any_selected) {
            l->eof = false;
            l->dts = MP_NOPTS_VALUE;
            TA_FREEP(&l->next);
        }
        apply_selection(l);
    }
}

static void d_switched_tracks(struct demuxer *demuxer)
{
    reselect(demuxer);
}

static void seek_lane(struct demuxer *demuxer, struct lane *l, double pts,
                      int flags)
{
    struct priv *p = demuxer->priv;
    struct variant *v = lane_var(l);
    if (!ensure_playlist(demuxer, v))
        return;
    struct lane *main = p->lanes[0];
    double base = l->have_base ? l->base : main->have_base ? main->base : 0;
    double rel = pts - base;
    int idx = v->pl->num - 1;
    for (int n = 0; n < v->pl->num; n++) {
        if (rel < v->pl->segs[n].start + v->pl->segs[n].dur) {
            idx = n;
            break;
        }
    }
    drop_current(l);
    TA_FREEP(&l->next);
    l->seq = v->pl->first_seq + idx;
    l->eof = false;
    l->dts = MP_NOPTS_VALUE;
    l->pending_discont = false;
    l->ts_offset = 0;
    if (!start_segment(demuxer, l))
        return;
    demux_seek(l->d, pts, flags);
}

static void d_seek(struct demuxer *demuxer, double pts, int flags)
{
    struct priv *p = demuxer->priv;
    if (!demuxer->seekable)
        return;
    if (flags & SEEK_FACTOR)
        pts = pts * p->duration;
    flags &= SEEK_FORWARD | SEEK_HR;
    for (int n = 0; n < p->num_lanes; n++) {
        if (p->lanes[n]->any_selected)
            seek_lane(demuxer, p->lanes[n], pts, flags);
    }
}

static void d_close(struct demuxer *demuxer)
{
    struct priv *p = demuxer->priv;
    if (!p)
        return;
    for (int n = 0; n < p->num_lanes; n++) {
        struct lane *l = p->lanes[n];
        TA_FREEP(&l->next);
        drop_current(l);
        for (int i = 0; i < l->num_anchors; i++)
            demux_free(l->anchors[i]);
    }
}

static struct lane *new_lane(struct priv *p, bool main, struct variant **vars,
                             int num_vars)
{
    struct lane *l = talloc_zero(p, struct lane);
    l->main = main;
    l->vars = vars;
    l->num_vars = num_vars;
    l->dts = MP_NOPTS_VALUE;
    l->codecs = talloc_zero_array(l, struct mp_codec_params **, num_vars);
    for (int n = 0; n < num_vars; n++)
        l->codecs[n] = talloc_zero_array(l, struct mp_codec_params *, 16);
    MP_TARRAY_APPEND(p, p->lanes, p->num_lanes, l);
    return l;
}

static int initial_variant(struct demuxer *demuxer, struct master *m)
{
    struct MPOpts *o = mp_get_config_group(NULL, demuxer->global, &mp_opt_root);
    int limit = o->hls_bitrate;
    talloc_free(o);

    bool any_video = false;
    for (int n = 0; n < m->num_vars; n++)
        any_video |= m->vars[n]->video;

    int best = -1;
    bool best_ok = false;
    for (int n = 0; n < m->num_vars; n++) {
        struct variant *v = m->vars[n];
        if (v->video != any_video)
            continue;
        bool ok = (limit < 0 || v->bw <= limit) && var_allowed(demuxer, v);
        if (limit < 0 && best < 0) {
            best = n;
            continue;
        }
        if (limit < 0)
            continue;
        if (best < 0 || (ok && !best_ok) ||
            (ok && best_ok && v->bw > m->vars[best]->bw) ||
            (!ok && !best_ok && v->bw < m->vars[best]->bw))
        {
            best = n;
            best_ok = ok;
        }
    }
    return best;
}

static bool open_lane(struct demuxer *demuxer, struct lane *l)
{
    struct priv *p = demuxer->priv;
    struct variant *v = lane_var(l);
    if (!ensure_playlist(demuxer, v))
        return false;
    struct playlist *pl = v->pl;
    p->live |= !pl->endlist;
    int start = 0;
    if (!pl->endlist)
        start = MPMAX(pl->num - 3, 0);
    l->seq = pl->first_seq + start;
    struct seg *s = find_seg(pl, l->seq);
    int64_t t0 = mp_time_ns();
    struct demuxer *d = open_segment(demuxer, l, s);
    if (!d)
        return false;
    l->d = d;
    if (!bind_streams(demuxer, l, d, true))
        return false;
    note_codecs(l, d);
    account(l, d, t0);
    l->base = d->start_time - s->start;
    l->have_base = true;
    l->opened_any = true;
    return true;
}

static int d_open(struct demuxer *demuxer, enum demux_check check)
{
    struct MPOpts *o = mp_get_config_group(NULL, demuxer->global, &mp_opt_root);
    bool enabled = o->hls_native;
    talloc_free(o);
    if (!enabled || !demuxer->stream || demuxer->depth > 0)
        return -1;

    char probe[16384];
    int len = stream_read_peek(demuxer->stream, probe, sizeof(probe));
    bstr head = {probe, len};
    if (!bstr_startswith0(bstr_lstrip(head), "#EXTM3U") &&
        !(head.len >= 3 && !memcmp(probe, "\xEF\xBB\xBF#EXTM3U", 10)))
        return -1;
    if (bstr_find0(head, "#EXT-X-STREAM-INF") < 0 &&
        bstr_find0(head, "#EXT-X-TARGETDURATION") < 0)
        return -1;

    struct priv *p = demuxer->priv = talloc_zero(demuxer, struct priv);
    p->base_url = demuxer->stream->url;

    bstr text = stream_read_complete(demuxer->stream, p, 8 * 1024 * 1024);
    if (!text.len)
        return -1;
    void *own = talloc_new(p);
    struct playlist *first = NULL;
    if (!parse_playlist(own, demuxer, text, p->base_url, &first, &p->master))
        return -1;
    if (p->master.has_subs)
        return -1;
    if (first) {
        for (int n = 1; n < first->num; n++) {
            if (first->segs[n].discont && first->endlist)
                return -1;
        }
    }

    struct lane *main;
    if (first) {
        struct variant *v = talloc_zero(p, struct variant);
        v->url = p->base_url;
        v->pl = first;
        v->video = true;
        struct variant **vars = talloc_zero_array(p, struct variant *, 1);
        vars[0] = v;
        main = new_lane(p, true, vars, 1);
    } else {
        int start = initial_variant(demuxer, &p->master);
        if (start < 0)
            return -1;
        main = new_lane(p, true, p->master.vars, p->master.num_vars);
        main->cur = start;
    }
    if (!open_lane(demuxer, main)) {
        MP_ERR(demuxer, "Failed to open the first segment.\n");
        goto fail;
    }

    char *group = lane_var(main)->audio;
    for (int n = 0; n < p->master.num_media; n++) {
        struct media *m = &p->master.media[n];
        if (!m->url || !group || strcmp(m->group, group))
            continue;
        int num = 0;
        struct variant **vars = NULL;
        int cur = 0;
        for (int i = 0; i < p->master.num_media; i++) {
            struct media *o2 = &p->master.media[i];
            if (!o2->url || strcmp(o2->name ? o2->name : "", m->name ? m->name : ""))
                continue;
            struct variant *v = talloc_zero(p, struct variant);
            v->url = o2->url;
            v->audio = o2->group;
            if (!strcmp(o2->group, group))
                cur = num;
            MP_TARRAY_APPEND(p, vars, num, v);
        }
        struct lane *l = new_lane(p, false, vars, num);
        l->cur = cur;
        if (!open_lane(demuxer, l)) {
            MP_WARN(demuxer, "Skipping audio rendition '%s'.\n", m->name);
            p->num_lanes--;
            continue;
        }
        if (l->num_vs) {
            struct sh_stream *sh = l->vs[0]->sh;
            sh->lang = m->lang;
            sh->title = m->name;
            sh->default_track = m->def;
        }
    }

    for (int n = 0; n < p->num_lanes; n++) {
        struct lane *l = p->lanes[n];
        if (l->num_vs)
            continue;
        MP_ERR(demuxer, "Lane without usable streams.\n");
        goto fail;
    }

    struct playlist *mpl = lane_var(main)->pl;
    p->duration = mpl->endlist ? mpl->total : -1;
    demuxer->duration = p->duration;
    demuxer->start_time = main->d ? main->d->start_time : 0;
    demuxer->seekable = !p->live;
    demuxer->partially_seekable = false;
    demuxer->is_network = true;
    demuxer->is_streaming = true;
    demuxer->filetype = "hls";
    demuxer->fully_read = false;

    reselect(demuxer);
    return 0;

fail:
    d_close(demuxer);
    return -1;
}

const demuxer_desc_t demuxer_desc_hls = {
    .name = "hls",
    .desc = "HLS playlist",
    .open = d_open,
    .read_packet = d_read_packet,
    .close = d_close,
    .seek = d_seek,
    .switched_tracks = d_switched_tracks,
};
