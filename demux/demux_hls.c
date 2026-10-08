#include <string.h>

#include "common/common.h"
#include "common/msg.h"
#include "misc/bstr.h"
#include "options/m_config.h"
#include "options/options.h"
#include "stream/stream.h"

#include "demux.h"
#include "demux_adaptive.h"

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
    char *key_url = NULL;
    unsigned char iv[16] = {0};
    bool has_iv = false;
    char *pending_stream_inf = NULL;
    struct part *pend = NULL;
    int num_pend = 0;

    while (text.len) {
        bstr line = bstr_strip(bstr_getline(text, &text));
        if (!line.len)
            continue;
        if (line.start[0] != '#') {
            char *abs = ad_resolve_url(ctx, url, bstrto0(ctx, line));
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
                .key_url = key_url,
                .has_iv = has_iv,
                .seq = pl->first_seq + pl->num,
                .start = pl->total,
            };
            memcpy(s.iv, iv, sizeof(iv));
            s.parts = pend;
            s.num_parts = num_pend;
            pend = NULL;
            num_pend = 0;
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
        } else if (bstr_eatstart0(&tag, "#EXT-X-PART-INF:")) {
            char *t = attr_get(ctx, tag, "PART-TARGET");
            pl->part_target = t ? atof(t) : 0;
        } else if (bstr_eatstart0(&tag, "#EXT-X-SERVER-CONTROL:")) {
            char *b = attr_get(ctx, tag, "CAN-BLOCK-RELOAD");
            pl->can_block = b && !strcmp(b, "YES");
            char *hb = attr_get(ctx, tag, "PART-HOLD-BACK");
            pl->hold_back = hb ? atof(hb) : 0;
        } else if (bstr_eatstart0(&tag, "#EXT-X-PART:")) {
            if (attr_get(ctx, tag, "BYTERANGE"))
                goto unsupported;
            char *u = attr_get(ctx, tag, "URI");
            char *dur = attr_get(ctx, tag, "DURATION");
            char *ind = attr_get(ctx, tag, "INDEPENDENT");
            if (u && dur && map_url && !key_url) {
                struct part pt = {
                    .url = ad_resolve_url(ctx, url, u),
                    .dur = atof(dur),
                    .indep = ind && !strcmp(ind, "YES"),
                };
                MP_TARRAY_APPEND(ctx, pend, num_pend, pt);
            }
        } else if (bstr_eatstart0(&tag, "#EXT-X-MAP:")) {
            if (attr_get(ctx, tag, "BYTERANGE"))
                goto unsupported;
            char *u = attr_get(ctx, tag, "URI");
            map_url = u ? ad_resolve_url(ctx, url, u) : NULL;
        } else if (bstr_eatstart0(&tag, "#EXT-X-KEY:")) {
            char *m = attr_get(ctx, tag, "METHOD");
            char *fmt = attr_get(ctx, tag, "KEYFORMAT");
            if (!m || !strcmp(m, "NONE")) {
                key_url = NULL;
                has_iv = false;
            } else if (!strcmp(m, "AES-128") && (!fmt || !strcmp(fmt, "identity"))) {
                char *u = attr_get(ctx, tag, "URI");
                if (!u)
                    goto unsupported;
                key_url = ad_resolve_url(ctx, url, u);
                char *iv_s = attr_get(ctx, tag, "IV");
                has_iv = iv_s && strlen(iv_s) > 2;
                memset(iv, 0, sizeof(iv));
                if (has_iv) {
                    const char *hex = iv_s + 2;
                    size_t n = strlen(hex);
                    if (n > 32)
                        goto unsupported;
                    for (size_t k = 0; k < n; k++) {
                        int c = hex[n - 1 - k];
                        int val = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
                        iv[15 - k / 2] |= (k & 1) ? val << 4 : val;
                    }
                }
            } else {
                goto unsupported;
            }
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
            m.url = u ? ad_resolve_url(ctx, url, u) : NULL;
            char *def = attr_get(ctx, tag, "DEFAULT");
            m.def = def && !strcmp(def, "YES");
            MP_TARRAY_APPEND(ctx, master->media, master->num_media, m);
        }
    }

    if (num_pend && !is_master) {
        struct seg s = {
            .map_url = map_url,
            .seq = pl->first_seq + pl->num,
            .start = pl->total,
            .parts = pend,
            .num_parts = num_pend,
        };
        for (int n = 0; n < num_pend; n++)
            s.dur += pend[n].dur;
        MP_TARRAY_APPEND(pl, pl->segs, pl->num, s);
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


static struct playlist *load_playlist(struct demuxer *demuxer, const char *url,
                                      int64_t msn, int part)
{
    void *tmp = talloc_new(NULL);
    void *own = talloc_new(NULL);
    char *full = (char *)url;
    if (msn >= 0) {
        full = talloc_asprintf(tmp, "%s%c_HLS_msn=%lld&_HLS_part=%d", url,
                               strchr(url, '?') ? '&' : '?', (long long)msn, part);
    }
    bstr text = ad_fetch(demuxer, tmp, full, 8 * 1024 * 1024);
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


static struct playlist *hls_load(struct demuxer *demuxer, struct variant *v,
                                 int64_t msn, int part)
{
    return load_playlist(demuxer, v->url, msn, part);
}

static const struct ad_ops hls_ops = {
    .load = hls_load,
};

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
        !(head.len >= 10 && !memcmp(probe, "\xEF\xBB\xBF#EXTM3U", 10)))
        return -1;
    if (bstr_find0(head, "#EXT-X-STREAM-INF") < 0 &&
        bstr_find0(head, "#EXT-X-TARGETDURATION") < 0)
        return -1;

    void *ctx = talloc_new(demuxer);
    char *base_url = talloc_strdup(ctx, demuxer->stream->url);
    struct master *m = talloc_zero(ctx, struct master);

    bstr text = stream_read_complete(demuxer->stream, ctx, 8 * 1024 * 1024);
    if (!text.len)
        return -1;
    void *own = talloc_new(ctx);
    struct playlist *first = NULL;
    if (!parse_playlist(own, demuxer, text, base_url, &first, m))
        return -1;
    if (m->has_subs)
        return -1;
    if (first) {
        for (int n = 1; n < first->num; n++) {
            if (first->segs[n].discont && first->endlist)
                return -1;
        }
    }

    struct ad_track *tracks = NULL;
    int num_tracks = 0;
    struct ad_track main = {.main = true};
    if (first) {
        struct variant *v = talloc_zero(ctx, struct variant);
        v->url = base_url;
        v->pl = first;
        v->video = true;
        main.vars = talloc_zero_array(ctx, struct variant *, 1);
        main.vars[0] = v;
        main.num_vars = 1;
    } else {
        main.cur = ad_initial_variant(demuxer, m->vars, m->num_vars);
        if (main.cur < 0)
            return -1;
        main.vars = m->vars;
        main.num_vars = m->num_vars;
    }
    MP_TARRAY_APPEND(ctx, tracks, num_tracks, main);

    char *group = main.vars[main.cur]->audio;
    for (int n = 0; n < m->num_media; n++) {
        struct media *md = &m->media[n];
        if (!md->url || !group || strcmp(md->group, group))
            continue;
        struct ad_track t = {.lang = md->lang, .title = md->name, .def = md->def};
        for (int i = 0; i < m->num_media; i++) {
            struct media *o2 = &m->media[i];
            if (!o2->url || strcmp(o2->name ? o2->name : "", md->name ? md->name : ""))
                continue;
            struct variant *v = talloc_zero(ctx, struct variant);
            v->url = o2->url;
            v->audio = o2->group;
            if (!strcmp(o2->group, group))
                t.cur = t.num_vars;
            MP_TARRAY_APPEND(ctx, t.vars, t.num_vars, v);
        }
        MP_TARRAY_APPEND(ctx, tracks, num_tracks, t);
    }

    return ad_open(demuxer, &hls_ops, ctx, tracks, num_tracks, "hls");
}

const demuxer_desc_t demuxer_desc_hls = {
    .name = "hls",
    .desc = "HLS playlist",
    .open = d_open,
    .read_packet = ad_read_packet,
    .close = ad_close,
    .seek = ad_seek,
    .switched_tracks = ad_switched_tracks,
};
