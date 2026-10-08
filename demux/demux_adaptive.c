#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include "common/common.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "options/options.h"
#include "osdep/timer.h"
#include "stream/stream.h"

#include "demux.h"
#include "demux_adaptive.h"
#include "packet.h"
#include "stheader.h"

struct vstream {
    struct sh_stream *sh;
    bool selected;
};

struct lane {
    bool main;
    bool indep;
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
    const struct ad_ops *ops;
    void *front;
    struct lane **lanes;
    int num_lanes;
    double duration;
    bool live;
    double rate_fast, rate_slow;
};

void *ad_front(struct demuxer *demuxer)
{
    struct priv *p = demuxer->priv;
    return p->front;
}

char *ad_resolve_url(void *ctx, const char *base, const char *ref)
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


bstr ad_fetch(struct demuxer *demuxer, void *ctx, const char *url, int max)
{
    struct stream *s = stream_create(url, STREAM_READ | demuxer->stream_origin,
                                     demuxer->cancel, demuxer->global);
    if (!s)
        return (bstr){0};
    bstr res = stream_read_complete(s, ctx, max);
    free_stream(s);
    return res;
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
    struct priv *p = demuxer->priv;
    v->pl = p->ops->load(demuxer, v);
    if (!v->pl)
        v->bad = true;
    return v->pl;
}

static bool reload_live(struct demuxer *demuxer, struct lane *l)
{
    struct variant *v = lane_var(l);
    struct priv *p = demuxer->priv;
    struct playlist *pl = p->ops->load(demuxer, v);
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
            l->init = ad_fetch(demuxer, l, s->map_url, 2 * 1024 * 1024);
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

bool ad_var_allowed(struct demuxer *demuxer, struct variant *v)
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
        if (v->bad || v->video != cur->video || !ad_var_allowed(demuxer, v))
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

bool ad_read_packet(struct demuxer *demuxer, struct demux_packet **out)
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

void ad_switched_tracks(struct demuxer *demuxer)
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

void ad_seek(struct demuxer *demuxer, double pts, int flags)
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

void ad_close(struct demuxer *demuxer)
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


int ad_open(struct demuxer *demuxer, const struct ad_ops *ops, void *front,
            struct ad_track *tracks, int num_tracks, const char *filetype)
{
    struct priv *p = demuxer->priv = talloc_zero(demuxer, struct priv);
    p->ops = ops;
    p->front = front;

    struct lane *main = NULL;
    for (int n = 0; n < num_tracks; n++) {
        struct ad_track *t = &tracks[n];
        struct lane *l = new_lane(p, t->main, t->vars, t->num_vars);
        l->cur = t->cur;
        l->indep = t->indep;
        if (!open_lane(demuxer, l)) {
            if (t->main) {
                MP_ERR(demuxer, "Failed to open the first segment.\n");
                goto fail;
            }
            MP_WARN(demuxer, "Skipping track '%s'.\n", t->title ? t->title : "?");
            p->num_lanes--;
            continue;
        }
        if (t->main)
            main = l;
        if (!t->main && l->num_vs) {
            struct sh_stream *sh = l->vs[0]->sh;
            sh->lang = t->lang;
            sh->title = t->title;
            sh->default_track = t->def;
        }
    }
    if (!main)
        goto fail;

    for (int n = 0; n < p->num_lanes; n++) {
        if (!p->lanes[n]->num_vs) {
            MP_ERR(demuxer, "Lane without usable streams.\n");
            goto fail;
        }
    }

    struct playlist *mpl = lane_var(main)->pl;
    p->duration = mpl->endlist ? mpl->total : -1;
    demuxer->duration = p->duration;
    demuxer->start_time = main->d ? main->d->start_time : 0;
    demuxer->seekable = !p->live;
    demuxer->partially_seekable = false;
    demuxer->is_network = true;
    demuxer->is_streaming = true;
    demuxer->filetype = filetype;
    demuxer->fully_read = false;

    reselect(demuxer);
    return 0;

fail:
    ad_close(demuxer);
    return -1;
}
