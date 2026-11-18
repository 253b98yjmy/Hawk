/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "blocker/hk_filter_store.h"
#include "base/hk_log.h"
#include "base/hk_mem.h"
#include "plat/hk_plat.h"
#include "plat/hk_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* README D3. The lists are downloaded at run time, not redistributed with Hawk. */
const hk_filter_list_info hk_default_filter_lists[] = {
    {"easylist", "https://easylist.to/easylist/easylist.txt"},
    {"easyprivacy", "https://easylist.to/easylist/easyprivacy.txt"},
    {"ublock-filters", "https://ublockorigin.github.io/uAssets/filters/filters.txt"},
    {"ublock-privacy", "https://ublockorigin.github.io/uAssets/filters/privacy.txt"},
    {"ublock-unbreak", "https://ublockorigin.github.io/uAssets/filters/unbreak.txt"},
    {"adguard-japanese", "https://filters.adtidy.org/extension/ublock/filters/7.txt"},
    {"peter-lowe", "https://pgl.yoyo.org/adservers/serverlist.php?hostformat=hosts&showintro=1&mimetype=plaintext"},
};
const size_t hk_default_filter_list_count = HK_ARRAY_LEN(hk_default_filter_lists);

#define ENGINE_STAMP "hawk-filters-3" /* bump when the compiler's output changes */

enum { MAX_EXTRA = 8 };
static char g_extra[MAX_EXTRA][512]; /* --filters files, kept for rebuilds after updates */
static int g_nextra;

struct hk_filter_store {
    hk_blocker *blocker;
    hk_mapped_file map;
    hk_charbuf image;
};

typedef HK_VEC(char *) path_vec;

static void add_path(void *user, const char *path)
{
    path_vec *v = user;
    size_t n = strlen(path);
    char *p = hk_alloc(n + 1);
    memcpy(p, path, n + 1);
    hk_vec_push(v, p);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void free_paths(path_vec *v)
{
    for (uint32_t i = 0; i < v->len; i++) hk_free(v->data[i], strlen(v->data[i]) + 1);
    hk_vec_free(v);
}

static hk_str base_name(const char *path)
{
    const char *s = strrchr(path, '\\');
    const char *t = strrchr(path, '/');
    if (t > s) s = t;
    s = s ? s + 1 : path;
    size_t n = strlen(s);
    if (n > 4 && strcmp(s + n - 4, ".txt") == 0) n -= 4;
    return (hk_str){s, n};
}

static void list_paths(const char *data_dir, path_vec *out)
{
    if (data_dir) {
        char dir[600];
        snprintf(dir, sizeof dir, "%s\\filters", data_dir);
        hk_plat_list_files(dir, ".txt", add_path, out);
    }
    for (int i = 0; i < g_nextra; i++) add_path(out, g_extra[i]);
    qsort(out->data, out->len, sizeof(char *), by_name);
}

static uint64_t stamp_of(const path_vec *paths)
{
    hk_charbuf b = {0};
    hk_charbuf_append(&b, ENGINE_STAMP, sizeof ENGINE_STAMP);
    for (uint32_t i = 0; i < paths->len; i++) {
        uint64_t s = hk_plat_file_stamp(paths->data[i]);
        hk_charbuf_append(&b, paths->data[i], strlen(paths->data[i]) + 1);
        hk_charbuf_append(&b, (const char *)&s, sizeof s);
    }
    uint64_t h = hk_hash64(b.data, b.len);
    hk_vec_free(&b);
    return h ? h : 1;
}

hk_filter_store *hk_filter_store_open(const char *data_dir, const char *const *extra, int nextra)
{
    for (int i = 0; i < nextra && g_nextra < MAX_EXTRA; i++)
        snprintf(g_extra[g_nextra++], sizeof g_extra[0], "%s", extra[i]);
    hk_filter_store *s = hk_alloc_zero(sizeof *s);
    path_vec paths = {0};
    list_paths(data_dir, &paths);
    uint64_t stamp = stamp_of(&paths);
    char bin[600] = "";
    if (data_dir) snprintf(bin, sizeof bin, "%s\\filters.bin", data_dir);

    int64_t t0 = hk_plat_now_ns();
    if (bin[0] && hk_plat_map_file(bin, &s->map)) {
        if (hk_blocker_image_stamp(s->map.data, s->map.size) == stamp)
            s->blocker = hk_blocker_open(s->map.data, s->map.size);
        if (s->blocker) {
            HK_LOGI("filters.bin: %u filters, %.1f KiB, mapped in %.2f ms", hk_blocker_count(s->blocker),
                    (double)s->map.size / 1024.0, (double)(hk_plat_now_ns() - t0) / 1e6);
            free_paths(&paths);
            return s;
        }
        hk_plat_unmap_file(&s->map);
    }

    hk_filter_compiler *c = hk_filter_compiler_create();
    for (uint32_t i = 0; i < paths.len; i++) {
        hk_charbuf text = {0};
        if (hk_plat_read_file(paths.data[i], &text) == HK_OK)
            hk_filter_compiler_add_list(c, base_name(paths.data[i]), (hk_str){text.data, text.len});
        else HK_LOGW("cannot read filter list %s", paths.data[i]);
        hk_vec_free(&text);
    }
    hk_filter_stats st;
    hk_filter_compiler_finish(c, stamp, &s->image, &st);
    HK_LOGI("filters compiled in %.0f ms: %u network (%u host, %u untokenized, %u regex); dropped %u cosmetic, "
            "%u unsupported, %u invalid, %u duplicate, %u badfilter; %.1f KiB",
            (double)(hk_plat_now_ns() - t0) / 1e6, st.network, st.hosts, st.untokenized, st.regex, st.cosmetic,
            st.unsupported, st.invalid, st.duplicate, st.badfiltered, (double)s->image.len / 1024.0);
    if (bin[0] && paths.len && hk_plat_write_file_atomic(bin, s->image.data, s->image.len) != HK_OK)
        HK_LOGW("cannot write %s", bin);
    s->blocker = hk_blocker_open(s->image.data, s->image.len);
    if (!s->blocker) HK_LOGE("compiled filter image is invalid");
    free_paths(&paths);
    return s;
}

const hk_blocker *hk_filter_store_blocker(const hk_filter_store *s)
{
    return s ? s->blocker : NULL;
}

void hk_filter_store_release(void *store)
{
    hk_filter_store *s = store;
    if (!s) return;
    hk_blocker_close(s->blocker);
    hk_plat_unmap_file(&s->map);
    hk_vec_free(&s->image);
    hk_free(s, sizeof *s);
}

/* --- updates ------------------------------------------------------------------------------- */

static void list_path(const char *data_dir, const hk_filter_list_info *info, char *out, size_t cap)
{
    snprintf(out, cap, "%s\\filters\\%s.txt", data_dir, info->name);
}

bool hk_filter_lists_missing(const char *data_dir)
{
    if (!data_dir) return false;
    for (size_t i = 0; i < hk_default_filter_list_count; i++) {
        char path[600];
        list_path(data_dir, &hk_default_filter_lists[i], path, sizeof path);
        if (hk_plat_file_stamp(path)) return false;
    }
    return true;
}

/* "! Expires: 4 days (update frequency)" or "! Expires: 12 hours". */
static int64_t expiry_seconds(hk_str text)
{
    int64_t def = 4 * 86400;
    size_t pos = 0;
    for (int line = 0; line < 60 && pos < text.n; line++) {
        size_t eol = pos;
        while (eol < text.n && text.p[eol] != '\n') eol++;
        hk_str l = hk_str_trim_ascii_ws(hk_str_slice(text, pos, eol));
        pos = eol + 1;
        if (!hk_str_starts_with(l, HK_STR("! Expires:"))) continue;
        hk_str v = hk_str_trim_ascii_ws(hk_str_slice(l, 10, l.n));
        int64_t n = 0;
        size_t i = 0;
        while (i < v.n && v.p[i] >= '0' && v.p[i] <= '9') n = n * 10 + (v.p[i++] - '0');
        hk_str unit = hk_str_trim_ascii_ws(hk_str_slice(v, i, v.n));
        int64_t secs = hk_str_starts_with(unit, HK_STR("hour")) ? n * 3600 : n * 86400;
        if (secs < 86400) secs = 86400;
        if (secs > 14 * 86400) secs = 14 * 86400;
        return secs;
    }
    return def;
}

static bool needs_update(const char *path)
{
    int64_t age = hk_plat_file_age(path);
    if (age < 0) return true;
    if (age < 86400) return false; /* the shortest expiry: no need to read the file */
    hk_charbuf text = {0};
    bool due = true;
    if (hk_plat_read_file(path, &text) == HK_OK) due = age >= expiry_seconds((hk_str){text.data, text.len});
    hk_vec_free(&text);
    return due;
}

typedef struct update_job {
    hk_loader *loader;
    char data_dir[512];
    int pending;
    int updated, failed;
    void (*done)(void *user, const char *summary);
    void *user;
    hk_thread *thread;
    hk_filter_store *store;
    char summary[256];
} update_job;

typedef struct list_fetch {
    update_job *job;
    const hk_filter_list_info *info;
    int status;
    hk_charbuf body;
} list_fetch;

static void job_finished(void *arg)
{
    update_job *job = arg;
    hk_thread_join(job->thread);
    if (job->done) job->done(job->user, job->summary);
    hk_free(job, sizeof *job);
}

static void compile_thread(void *arg)
{
    update_job *job = arg;
    hk_filter_store *s = hk_filter_store_open(job->data_dir, NULL, 0);
    snprintf(job->summary, sizeof job->summary, "%d list(s) updated, %d failed; %u filters", job->updated,
             job->failed, hk_blocker_count(s->blocker));
    hk_loader_set_blocker(job->loader, s->blocker, hk_filter_store_release, s);
    hk_main_post(job_finished, job);
}

static void job_step_done(update_job *job)
{
    if (--job->pending > 0) return;
    if (job->updated == 0) {
        snprintf(job->summary, sizeof job->summary, "filter lists are up to date (%d failed)", job->failed);
        job->thread = NULL;
        if (job->done) job->done(job->user, job->summary);
        hk_free(job, sizeof *job);
        return;
    }
    job->thread = hk_thread_start("hawk-filters", compile_thread, job);
}

static void on_list_response(void *user, const hk_load_response *r)
{
    list_fetch *f = user;
    f->status = r->status;
}

static void on_list_data(void *user, const uint8_t *p, size_t n)
{
    list_fetch *f = user;
    if (f->body.len + n <= 64u * 1024 * 1024) hk_charbuf_append(&f->body, (const char *)p, n);
}

static bool looks_like_list(hk_str t)
{
    hk_str head = hk_str_trim_ascii_ws(hk_str_slice(t, 0, 512));
    return t.n > 16 && !(head.n && head.p[0] == '<'); /* not an HTML error page */
}

static void on_list_done(void *user, hk_status st, const char *message)
{
    list_fetch *f = user;
    update_job *job = f->job;
    hk_str body = {f->body.data, f->body.len};
    if (st == HK_OK && f->status == 200 && looks_like_list(body)) {
        char path[600];
        list_path(job->data_dir, f->info, path, sizeof path);
        if (hk_plat_write_file_atomic(path, body.p, body.n) == HK_OK) {
            job->updated++;
            HK_LOGI("filter list %s: %.1f KiB", f->info->name, (double)body.n / 1024.0);
        } else {
            job->failed++;
        }
    } else {
        job->failed++;
        HK_LOGW("filter list %s: %s (HTTP %d)", f->info->name, st == HK_OK ? "not a filter list" : message, f->status);
    }
    hk_vec_free(&f->body);
    hk_free(f, sizeof *f);
    job_step_done(job);
}

void hk_filter_lists_update(hk_loader *l, const char *data_dir, bool force, void (*done)(void *user, const char *summary),
                            void *user)
{
    if (!data_dir) return;
    update_job *job = hk_alloc_zero(sizeof *job);
    job->loader = l;
    snprintf(job->data_dir, sizeof job->data_dir, "%s", data_dir);
    job->done = done;
    job->user = user;
    char dir[600];
    snprintf(dir, sizeof dir, "%s\\filters", data_dir);
    hk_plat_make_dir(dir);
    job->pending = 1; /* held until every fetch has started */
    for (size_t i = 0; i < hk_default_filter_list_count; i++) {
        const hk_filter_list_info *info = &hk_default_filter_lists[i];
        char path[600];
        list_path(data_dir, info, path, sizeof path);
        if (!force && !needs_update(path)) continue;
        list_fetch *f = hk_alloc_zero(sizeof *f);
        f->job = job;
        f->info = info;
        job->pending++;
        hk_load_callbacks cb = {f, on_list_response, on_list_data, on_list_done, NULL};
        hk_loader_fetch(l, hk_str_from_cstr(info->url), HK_STR(""), HK_REQ_OTHER, &cb);
    }
    job_step_done(job);
}
