#pragma once

#include "misc/bstr.h"

struct demuxer;

struct part {
    char *url;
    double dur;
    bool indep;
};

struct seg {
    char *url;
    char *map_url;
    double dur;
    double start;
    int64_t seq;
    bool discont;
    char *key_url;
    unsigned char iv[16];
    bool has_iv;
    int64_t off, len;
    int64_t map_off, map_len;
    struct part *parts;
    int num_parts;
};

struct playlist {
    void *own;
    struct seg *segs;
    int num;
    int64_t first_seq;
    double target;
    double total;
    bool endlist;
    bool can_block;
    bool ll;
    double part_target;
    double hold_back;
};

struct variant {
    char *url;
    char *audio;
    char *subs;
    void *front;
    int bw, w, h;
    bool video;
    bool bad;
    struct playlist *pl;
};

struct ad_ops {
    struct playlist *(*load)(struct demuxer *demuxer, struct variant *v,
                             int64_t msn, int part);
};

struct ad_track {
    struct variant **vars;
    int num_vars;
    int cur;
    bool main;
    bool indep;
    bool sub;
    char *lang;
    char *title;
    bool def;
};

int ad_open(struct demuxer *demuxer, const struct ad_ops *ops, void *front,
            struct ad_track *tracks, int num_tracks, const char *filetype);
void *ad_front(struct demuxer *demuxer);
int ad_initial_variant(struct demuxer *demuxer, struct variant **vars, int num_vars);
bool ad_var_allowed(struct demuxer *demuxer, struct variant *v);
char *ad_resolve_url(void *ctx, const char *base, const char *ref);
bstr ad_fetch(struct demuxer *demuxer, void *ctx, const char *url, int max);
bstr ad_fetch_range(struct demuxer *demuxer, void *ctx, const char *url,
                    int64_t off, int64_t len);

bool ad_read_packet(struct demuxer *demuxer, struct demux_packet **out);
void ad_seek(struct demuxer *demuxer, double pts, int flags);
void ad_close(struct demuxer *demuxer);
void ad_switched_tracks(struct demuxer *demuxer);
