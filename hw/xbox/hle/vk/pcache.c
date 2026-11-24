/*
 * xbased HLE GPU — on-disk shader and pipeline caches (P3 Task 13).
 * - SPIR-V: <base>/hle_spirv/<title id>.pack, the title's shaders in one file,
 *   each keyed by the GLSL's fnv64 with its length and a second hash so a
 *   collision reads as a miss; legacy.pack, the loose files of before
 *   (hle_spirv-vk11/: SPIR-V 1.3, for a device below Vulkan 1.3).
 * - Pipelines: <base>/hle_pipeline_cache/<title id>.bin, the title's
 *   VkPipelineCache blob (the driver rejects a blob from another device or
 *   driver version), written by a worker thread: the blob is megabytes and
 *   the write syncs to flash (~110 ms), which on the render thread stalled a
 *   frame every 5 s. The cache is internally synchronized: the worker reads
 *   it while the render thread creates pipelines. Written when pipelines
 *   were compiled since (hle_pcache_compiled), at most once a minute, from a
 *   copy: the driver holds its cache's lock while it serializes, and a write
 *   of the live cache kept the render thread's pipeline lookups out for
 *   0.2-0.8 s at a time (NGB froze, then dropped the frames it caught up
 *   with). A pipeline compiled since the last write and lost with the
 *   process is built again by the next start's prebuild (its record,
 *   pipeasync.c, is written apart). One cache for every title (until
 *   1.0.0's) grew to 515 MB on the RP5 over 45 titles: each game held all of
 *   it in memory and wrote all of it back.
 */
#include "hw/xbox/hle/base.h"
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include "hw/xbox/hle/hle.h"

#include "hw/xbox/hle/vk/pcache.h"
#include "hw/xbox/hle/vk/pipeasync.h"
#include "hw/xbox/hle/vk/vkdev.h"

#define T (g_hvk.t)
#define SPV_MAGIC   0x56505348u     /* "HSPV" */
#define SAVE_EVERY_US (60 * G_USEC_PER_SEC)

static struct {
    VkPipelineCache cache;
    char *dir, *blob;
    uint64_t added, saved;          /* pipelines compiled / at the last write */
    int64_t last_save;
    uint64_t spv_hits, spv_misses;
    HleThread th;
    HleSem go;
    bool started, busy, stop;       /* busy: a write in flight */
} g_pc;

static void spv_packs_open(void);
static void spv_packs_close(void);

static uint64_t fnv64(const void *p, size_t n, uint64_t h)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 0x100000001b3ull;
    }
    return h;
}

void hle_pcache_init(void)
{
    const char *base = hle_host_base_path();
    gchar *data = NULL;
    gsize len = 0;
    if (!hle_pipe_nocache()) {      /* else neither read nor written (pipeasync.h) */
        /* SPIR-V 1.3 for a device below Vulkan 1.3 apart */
        g_pc.dir = g_strdup_printf("%shle_spirv%s", base, g_hle_glsl_vk11 ? "-vk11" : "");
        g_autofree char *pdir = g_strdup_printf("%shle_pipeline_cache", base);
        g_pc.blob = g_strdup_printf("%s/%08x.bin", pdir, g_hle.title.xbe.title_id);
        g_mkdir_with_parents(g_pc.dir, 0700);
        g_mkdir_with_parents(pdir, 0700);
        g_file_get_contents(g_pc.blob, &data, &len, NULL);
        /* every title's, before: its pipelines come back by each title's
         * prebuild, compiled once into the title's own */
        g_autofree char *shared = g_strdup_printf("%shle_pipeline_cache.bin", base);
        struct stat sb;
        if (stat(shared, &sb) == 0 && remove(shared) == 0) {
            hle_log("xbased-hle-pipe", "the shared pipeline cache removed (%lld bytes): one per "
                    "title now", (long long)sb.st_size);
        }
    }
    VkPipelineCacheCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = data ? len : 0, .pInitialData = data };
    if (T.vkCreatePipelineCache(g_hvk.dev, &ci, NULL, &g_pc.cache) != VK_SUCCESS && data) {
        ci.initialDataSize = 0;     /* a blob the driver refuses: start empty */
        ci.pInitialData = NULL;
        T.vkCreatePipelineCache(g_hvk.dev, &ci, NULL, &g_pc.cache);
    }
    hle_log("xbased-hle-pipe", "pipeline cache %s: %zu bytes", g_pc.blob ? g_pc.blob : "(none)",
            data ? (size_t)len : 0);
    g_free(data);
    g_pc.last_save = g_get_monotonic_time();
    spv_packs_open();
}

VkPipelineCache hle_pcache_get(void)
{
    return g_pc.cache;
}

void hle_pcache_compiled(void)
{
    hle_atomic_inc(&g_pc.added);       /* the render thread and the pipeline workers */
}

static void write_blob(void)
{
    if (!g_pc.blob) {
        return;
    }
    /* a copy to serialize (Mesa's merge takes references to the entries):
     * the live cache is locked for the merge alone */
    const int64_t t0 = g_get_monotonic_time();
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    VkPipelineCache copy = VK_NULL_HANDLE;
    if (T.vkCreatePipelineCache(g_hvk.dev, &ci, NULL, &copy) != VK_SUCCESS) {
        return;
    }
    const bool merged = T.vkMergePipelineCaches(g_hvk.dev, copy, 1, &g_pc.cache) == VK_SUCCESS;
    const int64_t t1 = g_get_monotonic_time();
    size_t n = 0;
    void *buf = NULL;
    if (merged && T.vkGetPipelineCacheData(g_hvk.dev, copy, &n, NULL) == VK_SUCCESS && n) {
        buf = g_malloc(n);
        if (T.vkGetPipelineCacheData(g_hvk.dev, copy, &n, buf) != VK_SUCCESS) {
            g_free(buf);
            buf = NULL;
        }
    }
    T.vkDestroyPipelineCache(g_hvk.dev, copy, NULL);
    if (buf) {
        const int64_t t2 = g_get_monotonic_time();
        g_file_set_contents(g_pc.blob, buf, n, NULL);   /* temp file + rename */
        hle_log("xbased-hle-pipe", "pipeline cache saved: %zu bytes (merged in %.1f ms, "
                "serialized in %.1f ms, written in %.1f ms), spirv hits=%llu misses=%llu", n,
                (t1 - t0) / 1000.0, (t2 - t1) / 1000.0, (g_get_monotonic_time() - t2) / 1000.0,
                (unsigned long long)g_pc.spv_hits, (unsigned long long)g_pc.spv_misses);
    }
    g_free(buf);
}

static void *save_thread(void *opaque)
{
    for (;;) {
        hle_sem_wait(&g_pc.go);
        if (hle_atomic_read(&g_pc.stop)) {
            return NULL;
        }
        write_blob();
        hle_atomic_set(&g_pc.busy, false);
    }
}

/* Render thread (swaps): a write when pipelines were added since the last
 * one, at most every SAVE_EVERY_US, on the worker; force: now, here, after
 * the worker is done (destroy). */
void hle_pcache_save(bool force)
{
    int64_t now = g_get_monotonic_time();
    if (!g_pc.cache || g_pc.added == g_pc.saved ||
        (!force && now - g_pc.last_save < SAVE_EVERY_US)) {
        return;
    }
    if (force) {
        write_blob();
    } else {
        if (hle_atomic_read(&g_pc.busy)) {
            return;                     /* the previous write is still going */
        }
        if (!g_pc.started) {
            hle_sem_init(&g_pc.go, 0);
            hle_thread_create(&g_pc.th, "hle.pcache", save_thread, NULL,
                               HLE_THREAD_JOINABLE);
            g_pc.started = true;
        }
        hle_atomic_set(&g_pc.busy, true);
        hle_sem_post(&g_pc.go);
    }
    g_pc.saved = g_pc.added;
    g_pc.last_save = now;
}

void hle_pcache_destroy(void)
{
    if (g_pc.started) {
        hle_atomic_set(&g_pc.stop, true);
        hle_sem_post(&g_pc.go);
        hle_thread_join(&g_pc.th);
        g_pc.started = false;
    }
    hle_pcache_save(true);
    if (g_pc.cache) {
        T.vkDestroyPipelineCache(g_hvk.dev, g_pc.cache, NULL);
        g_pc.cache = VK_NULL_HANDLE;
    }
    spv_packs_close();
    g_free(g_pc.dir);
    g_free(g_pc.blob);
    g_pc.dir = g_pc.blob = NULL;
}

/* ---- SPIR-V packs ----
 * The title's shaders in one file, <dir>/<title id>.pack: a header, then a
 * record per shader as it compiles (PackRec, then the words), appended, and
 * an index in memory. One file a shader (before 1.0.15) made thousands of
 * small files: a filesystem's clusters made 400 MB of them take gigabytes,
 * and each look-up was a file to open (slow on a hard disk). Those loose
 * files go into legacy.pack the first time (every title's, read after the
 * title's own), and are removed. A record cut short (the process ended in
 * its write) ends the walk and is cut off. */
#define PACK_MAGIC  0x4b505348u     /* "HSPK" */
#define PACK_VER    1u

typedef struct SpvHeader {          /* a loose file's, before */
    uint32_t magic, src_len;
    uint64_t check;                 /* second hash of the source */
    uint32_t nwords, pad;
} SpvHeader;

typedef struct PackRec {
    uint32_t magic, src_len;        /* SPV_MAGIC */
    uint64_t key, check;            /* the source's hash (^ stage), a second one */
    uint32_t nwords, pad;
} PackRec;

typedef struct Pack {
    uint8_t *data;                  /* the records (the file's, then the appended) */
    size_t len, cap;
    uint64_t *keys;                 /* open addressing: key, record offset + 1 (0 free) */
    size_t *offs;
    size_t slots, n;
    FILE *f;                        /* appends */
    char *path;
} Pack;

static HleMutex g_pack_mx;
static Pack g_title_pack, g_legacy_pack;

static void pack_index(Pack *p, uint64_t key, size_t off)
{
    if ((p->n + 1) * 2 > p->slots) {
        const size_t ns = p->slots ? p->slots * 2 : 1024;
        uint64_t *nk = g_new0(uint64_t, ns);
        size_t *no = g_new0(size_t, ns);
        for (size_t i = 0; i < p->slots; i++) {
            if (p->offs[i]) {
                size_t j = (size_t)(p->keys[i] * 0x9e3779b97f4a7c15ull) & (ns - 1);
                while (no[j]) {
                    j = (j + 1) & (ns - 1);
                }
                nk[j] = p->keys[i];
                no[j] = p->offs[i];
            }
        }
        g_free(p->keys);
        g_free(p->offs);
        p->keys = nk;
        p->offs = no;
        p->slots = ns;
    }
    size_t j = (size_t)(key * 0x9e3779b97f4a7c15ull) & (p->slots - 1);
    while (p->offs[j] && p->keys[j] != key) {
        j = (j + 1) & (p->slots - 1);
    }
    if (!p->offs[j]) {
        p->n++;
    }
    p->keys[j] = key;
    p->offs[j] = off + 1;           /* a later record of the key wins */
}

static const PackRec *pack_find(const Pack *p, uint64_t key)
{
    if (!p->slots) {
        return NULL;
    }
    size_t j = (size_t)(key * 0x9e3779b97f4a7c15ull) & (p->slots - 1);
    while (p->offs[j]) {
        if (p->keys[j] == key) {
            return (const PackRec *)(p->data + p->offs[j] - 1);
        }
        j = (j + 1) & (p->slots - 1);
    }
    return NULL;
}

static void pack_grow(Pack *p, size_t more)
{
    if (p->len + more > p->cap) {
        p->cap = MAX(p->cap * 2, p->len + more + (1u << 20));
        p->data = g_realloc(p->data, p->cap);
    }
}

/* The pack at path read and indexed, opened for appends */
static void pack_open(Pack *p, const char *path)
{
    memset(p, 0, sizeof(*p));
    p->path = g_strdup(path);
    gchar *file = NULL;
    gsize n = 0;
    uint32_t head[2];
    size_t good = 0;                /* the valid records' end in the file */
    if (g_file_get_contents(path, &file, &n, NULL) && n >= sizeof(head)) {
        memcpy(head, file, sizeof(head));
        if (head[0] == PACK_MAGIC && head[1] == PACK_VER) {
            good = sizeof(head);
            while (good + sizeof(PackRec) <= n) {
                PackRec r;
                memcpy(&r, file + good, sizeof(r));
                const size_t sz = sizeof(r) + 4ull * r.nwords;
                if (r.magic != SPV_MAGIC || !r.nwords || r.nwords > (64u << 20) || good + sz > n) {
                    break;
                }
                good += sz;
            }
        }
    }
    if (good) {
        pack_grow(p, good - sizeof(head));
        memcpy(p->data, file + sizeof(head), good - sizeof(head));
        p->len = good - sizeof(head);
        for (size_t o = 0; o < p->len;) {
            const PackRec *r = (const PackRec *)(p->data + o);
            pack_index(p, r->key, o);
            o += sizeof(*r) + 4ull * r->nwords;
        }
    }
    if (!good || good != n) {
        /* new, unreadable or cut short: written again from what was good */
        head[0] = PACK_MAGIC;
        head[1] = PACK_VER;
        FILE *w = fopen(path, "wb");
        if (w) {
            fwrite(head, sizeof(head), 1, w);
            if (p->len) {
                fwrite(p->data, p->len, 1, w);
            }
            fclose(w);
        }
        if (n && good != n) {
            hle_log("xbased-hle-pipe", "spirv pack %s: %zu bytes past its last whole record cut off",
                    path, (size_t)(n - good));
        }
    }
    g_free(file);
    p->f = fopen(path, "ab");
}

static void pack_add(Pack *p, uint64_t key, uint64_t check, uint32_t src_len, const uint32_t *words,
                     uint32_t nwords)
{
    const PackRec r = { SPV_MAGIC, src_len, key, check, nwords, 0 };
    const size_t sz = sizeof(r) + 4ull * nwords;
    pack_grow(p, sz);
    memcpy(p->data + p->len, &r, sizeof(r));
    memcpy(p->data + p->len + sizeof(r), words, 4ull * nwords);
    pack_index(p, key, p->len);
    p->len += sz;
    if (p->f) {
        fwrite(p->data + p->len - sz, sz, 1, p->f);
        fflush(p->f);
    }
}

static void pack_close(Pack *p)
{
    if (p->f) {
        fclose(p->f);
    }
    g_free(p->data);
    g_free(p->keys);
    g_free(p->offs);
    g_free(p->path);
    memset(p, 0, sizeof(*p));
}

/* The loose files of before into legacy.pack, once (removed as they go in) */
static void spv_migrate(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    unsigned moved = 0;
    size_t bytes = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        unsigned long long key;
        int stage;
        char tail[8];
        if (sscanf(e->d_name, "%16llx-%d.%4s", &key, &stage, tail) != 3 || strcmp(tail, "spv")) {
            continue;
        }
        g_autofree char *path = g_strdup_printf("%s/%s", dir, e->d_name);
        gchar *data = NULL;
        gsize n = 0;
        if (g_file_get_contents(path, &data, &n, NULL) && n >= sizeof(SpvHeader)) {
            SpvHeader h;
            memcpy(&h, data, sizeof(h));
            if (h.magic == SPV_MAGIC && h.nwords && n == sizeof(h) + 4ull * h.nwords) {
                if (!pack_find(&g_legacy_pack, key)) {
                    pack_add(&g_legacy_pack, key, h.check, h.src_len,
                             (const uint32_t *)(data + sizeof(h)), h.nwords);
                }
                moved++;
                bytes += n;
            }
        }
        g_free(data);
        remove(path);
    }
    closedir(d);
    if (moved) {
        hle_log("xbased-hle-pipe", "spirv: %u loose files (%zu bytes) packed into %s", moved, bytes,
                g_legacy_pack.path);
    }
}

static void spv_packs_open(void)
{
    hle_mutex_init(&g_pack_mx);
    if (!g_pc.dir) {
        return;
    }
    g_autofree char *legacy = g_strdup_printf("%s/legacy.pack", g_pc.dir);
    g_autofree char *title = g_strdup_printf("%s/%08x.pack", g_pc.dir, g_hle.title.xbe.title_id);
    pack_open(&g_legacy_pack, legacy);
    spv_migrate(g_pc.dir);
    pack_open(&g_title_pack, title);
    hle_log("xbased-hle-pipe", "spirv packs: %s %zu shaders, legacy %zu", title, g_title_pack.n,
            g_legacy_pack.n);
}

static void spv_packs_close(void)
{
    hle_mutex_lock(&g_pack_mx);
    pack_close(&g_title_pack);
    pack_close(&g_legacy_pack);
    hle_mutex_unlock(&g_pack_mx);
}

/* src's SPIR-V from the packs, or NULL */
static uint32_t *spv_read(HleShaderStage stage, const char *src, size_t *nwords, uint64_t *key_out,
                          uint64_t *check_out)
{
    const size_t len = strlen(src);
    const uint64_t key = fnv64(src, len, 0xcbf29ce484222325ull) ^ (uint64_t)stage;
    const uint64_t check = fnv64(src, len, 0x84222325cbf29ce4ull);
    *key_out = key;
    *check_out = check;
    uint32_t *spv = NULL;
    hle_mutex_lock(&g_pack_mx);
    const Pack *packs[2] = { &g_title_pack, &g_legacy_pack };
    for (unsigned i = 0; i < 2 && !spv; i++) {
        const PackRec *r = pack_find(packs[i], key);
        if (r && r->src_len == len && r->check == check) {   /* else a collision: a miss */
            spv = g_malloc(4ull * r->nwords);
            memcpy(spv, r + 1, 4ull * r->nwords);
            *nwords = r->nwords;
        }
    }
    hle_mutex_unlock(&g_pack_mx);
    if (spv) {
        hle_atomic_inc(&g_pc.spv_hits);
    }
    return spv;
}

uint32_t *hle_spirv_peek(HleShaderStage stage, const char *src, size_t *nwords)
{
    uint64_t key, check;
    return spv_read(stage, src, nwords, &key, &check);
}

uint32_t *hle_spirv_get(HleShaderStage stage, const char *src, size_t *nwords)
{
    uint64_t key, check;
    uint32_t *spv = spv_read(stage, src, nwords, &key, &check);
    if (spv) {
        return spv;
    }
    /* any thread: the render thread, the pipeline workers, the prebuild.
     * glslang compiles in parallel (16.x: a pool allocator per thread, its
     * built-in tables made under its own lock), as the other callers of
     * hle_glsl_compile always did */
    spv = hle_glsl_compile(stage, src, nwords);
    hle_atomic_inc(&g_pc.spv_misses);
    if (spv && *nwords) {
        hle_mutex_lock(&g_pack_mx);
        if (g_title_pack.path) {
            pack_add(&g_title_pack, key, check, (uint32_t)strlen(src), spv, (uint32_t)*nwords);
        }
        hle_mutex_unlock(&g_pack_mx);
    }
    return spv;
}