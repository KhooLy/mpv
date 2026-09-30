#include "mpv/client.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "common/common.h"
#include "common/global.h"
#include "misc/node.h"
#include "mpv_talloc.h"
#include "dovi_stats.h"

static void destroy(void *p)
{
    struct mp_dovi_stats *s = p;
    mp_mutex_destroy(&s->lock);
}

void mp_dovi_stats_init(struct mpv_global *global)
{
    struct mp_dovi_stats *s = talloc_zero(global, struct mp_dovi_stats);
    mp_mutex_init(&s->lock);
    talloc_set_destructor(s, destroy);
    global->dovi = s;
    mp_dovi_stats_reset(s);
}

void mp_dovi_stats_reset(struct mp_dovi_stats *s)
{
    size_t off = offsetof(struct mp_dovi_stats, decoder);
    mp_mutex_lock(&s->lock);
    memset((char *)s + off, 0, sizeof(*s) - off);
    s->profile = -1;
    s->error_pts = -1;
    s->gpu_recent = -1;
    mp_mutex_unlock(&s->lock);
}

void mp_dovi_stats_error(struct mp_dovi_stats *s, double pts, const char *msg)
{
    if (s->error[0])
        return;
    snprintf(s->error, sizeof(s->error), "%s", msg);
    s->error_pts = pts;
}

void mp_dovi_stats_gpu(struct mp_dovi_stats *s, double ms)
{
    s->gpu_hist[MPMIN((int)(ms * 10), DOVI_GPU_BINS - 1)]++;
    s->gpu_n++;
    s->gpu_sum += ms;
    s->gpu_max = MPMAX(s->gpu_max, ms);
    s->gpu_recent = s->gpu_recent < 0 ? ms : s->gpu_recent * 0.95 + ms * 0.05;
}

static void add_str(struct mpv_node *m, const char *k, const char *v)
{
    if (v[0])
        node_map_add_string(m, k, v);
}

void mp_dovi_stats_live(struct mp_dovi_stats *s, struct mpv_node *res)
{
    mp_mutex_lock(&s->lock);
    node_init(res, MPV_FORMAT_NODE_MAP, NULL);
    add_str(res, "path", s->path);
    add_str(res, "output", s->output);
    if (s->l1[0] > 0) {
        struct mpv_node *l1 = node_map_add(res, "l1", MPV_FORMAT_NODE_MAP);
        node_map_add_double(l1, "max", s->l1[0]);
        node_map_add_double(l1, "avg", s->l1[1]);
        node_map_add_double(l1, "min", s->l1[2]);
    }
    add_str(res, "trim", s->trim);
    if (s->tm_dst > 0) {
        node_map_add_double(res, "tonemap-source", s->tm_src);
        node_map_add_double(res, "tonemap-target", s->tm_dst);
    }
    if (s->hdr10p[0] > 0) {
        struct mpv_node *h = node_map_add(res, "hdr10-plus", MPV_FORMAT_NODE_MAP);
        node_map_add_double(h, "peak", s->hdr10p[0]);
        node_map_add_double(h, "p50", s->hdr10p[1]);
        node_map_add_double(h, "p99", s->hdr10p[2]);
    }
    if (s->gpu_recent >= 0)
        node_map_add_double(res, "gpu-ms", s->gpu_recent);
    node_map_add_int64(res, "rpus", s->rpus);
    node_map_add_int64(res, "rpu-errors", s->rpu_errors);
    node_map_add_int64(res, "missing-rpu", s->missing);
    mp_mutex_unlock(&s->lock);
}

void mp_dovi_stats_summary(struct mp_dovi_stats *s, struct mpv_node *res)
{
    mp_mutex_lock(&s->lock);
    node_init(res, MPV_FORMAT_NODE_MAP, NULL);
    add_str(res, "path", s->path);
    add_str(res, "reason", s->reason);
    add_str(res, "decoder", s->decoder);
    add_str(res, "output", s->output);
    node_map_add_int64(res, "profile", s->profile);
    node_map_add_int64(res, "level", s->level);
    node_map_add_int64(res, "compatibility", s->compat);
    add_str(res, "el-type", s->el_type);
    add_str(res, "cm-version", s->cm);
    add_str(res, "trims", s->trims);
    add_str(res, "trim-used", s->trim);
    if (s->mastering_max > 0) {
        node_map_add_double(res, "mastering-max", s->mastering_max);
        node_map_add_double(res, "mastering-min", s->mastering_min);
    }
    if (s->max_cll > 0) {
        node_map_add_double(res, "max-cll", s->max_cll);
        node_map_add_double(res, "max-fall", s->max_fall);
    }
    if (s->l1_n) {
        node_map_add_double(res, "l1-peak", s->l1_peak);
        node_map_add_double(res, "l1-avg", s->l1_sum / s->l1_n);
    }
    node_map_add_int64(res, "rpus", s->rpus);
    node_map_add_int64(res, "rpu-errors", s->rpu_errors);
    node_map_add_int64(res, "scenes", s->scenes);
    node_map_add_int64(res, "converted", s->converted);
    node_map_add_int64(res, "convert-errors", s->convert_errors);
    node_map_add_int64(res, "frames-composed", s->frames);
    node_map_add_int64(res, "missing-rpu", s->missing);
    node_map_add_int64(res, "trim-frames", s->trim_frames);
    node_map_add_int64(res, "dropped-decoder", s->dropped_dec);
    node_map_add_int64(res, "dropped-vo", s->dropped_vo);
    if (s->gpu_n) {
        int64_t want = s->gpu_n * 99 / 100, n = 0;
        int i = 0;
        while (i < DOVI_GPU_BINS - 1 && (n += s->gpu_hist[i]) <= want)
            i++;
        node_map_add_double(res, "gpu-avg-ms", s->gpu_sum / s->gpu_n);
        node_map_add_double(res, "gpu-p99-ms", (i + 1) / 10.0);
        node_map_add_double(res, "gpu-max-ms", s->gpu_max);
    }
    if (s->error[0]) {
        node_map_add_string(res, "error", s->error);
        node_map_add_double(res, "error-time", s->error_pts);
    }
    mp_mutex_unlock(&s->lock);
}
