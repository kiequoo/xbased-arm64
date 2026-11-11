/*
 * Host test of the savestate's cache-drive section read as a state from
 * anywhere (native/state/cache.c, nc_cache_state_load). What the section
 * lists is created and written in the hard disk folder, with content from
 * the store beside the state: the folders and files it names have to stay
 * inside their partition's folder, and its counts inside the section. Each
 * case builds a section by hand, loads it on a hard disk folder of the
 * test's own, and looks at what is on the disk afterwards.
 *
 *   clang -O1 -g -fsanitize=address -I<repo> -I<deps>/xxhash tests/native/state/cache_test.c \
 *       native/state/cache.c native/state/state.c -lz -lpthread -o cache_test
 */
#include "native/state/sections.h"
#include "native/state/state.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* what cache.c asks the core for */
static char g_root[400], g_state[480];
const char *nc_io_hdd_root(void) { return g_root; }
const char *nc_state_path(void) { return g_state; }
unsigned nc_io_cache_partitions(void) { return 0; }
void nc_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }

static int g_fail;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                       \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static bool exists(const char *fmt, ...)
{
    char p[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p, sizeof(p), fmt, ap);
    va_end(ap);
    struct stat st;
    return stat(p, &st) == 0;
}

static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        char p[900];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", path, de->d_name);
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_tree(p);
        } else {
            unlink(p);
        }
    }
    if (d) {
        closedir(d);
    }
    rmdir(path);
}

/* The section's bytes, as cache.def lays them out: NcCacheState { nparts },
 * then per partition NcCachePart { part, raw_len, ndirs, nfiles }, its raw
 * head, its folders (a 256-byte path each) and its files (path, size, time,
 * content hash). */
static uint8_t g_sec[1 << 16];
static size_t g_len;

static void put32(uint32_t v)
{
    memcpy(g_sec + g_len, &v, 4);
    g_len += 4;
}

static void put_path(const char *p)
{
    memset(g_sec + g_len, 0, 256);
    snprintf((char *)g_sec + g_len, 256, "%s", p);
    g_len += 256;
}

static const uint8_t k_hash[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                   0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

/* A section with one partition (3): its folders, and one 5-byte file if named. */
static bool load(const char *const *dirs, unsigned ndirs, const char *file)
{
    g_len = 0;
    put32(1);
    put32(3);
    put32(0);
    put32(ndirs);
    put32(file ? 1 : 0);
    for (unsigned i = 0; i < ndirs; i++) {
        put_path(dirs[i]);
    }
    if (file) {
        put_path(file);
        const uint64_t size = 5, mtime = 1000000000ull * 1000000;
        memcpy(g_sec + g_len, &size, 8);
        memcpy(g_sec + g_len + 8, &mtime, 8);
        memcpy(g_sec + g_len + 16, k_hash, 16);
        g_len += 32;
    }
    NcRd r = {g_sec, g_len, 0, false};
    return nc_cache_state_load(&r, NC_STATE_V_CACHE);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* a line is out before the sanitizer may stop the program */
    const char *tmp = getenv("TMP");
    char top[300];
    snprintf(top, sizeof(top), "%s/cache_test_%d", tmp && tmp[0] ? tmp : ".", (int)getpid());
    snprintf(g_root, sizeof(g_root), "%s/hdd", top);
    snprintf(g_state, sizeof(g_state), "%s/states/slot1.hxs", top);
    char p[900];
    mkdir(top, 0775);
    mkdir(g_root, 0775);
    snprintf(p, sizeof(p), "%s/states", top);
    mkdir(p, 0775);
    snprintf(p, sizeof(p), "%s/states/blobs", top);
    mkdir(p, 0775);
    /* the store's copy of the 5-byte file every case below names */
    snprintf(p, sizeof(p), "%s/states/blobs/00112233445566778899aabbccddeeff", top);
    FILE *fp = fopen(p, "wb");
    CHECK(fp && fwrite("hello", 1, 5, fp) == 5);
    if (fp) {
        fclose(fp);
    }

    /* a drive as a state lists it: folders and a file inside the partition's folder */
    {
        const char *const dirs[] = {"data", "data/lang"};
        CHECK(load(dirs, 2, "data/lang/text.big"));
        CHECK(exists("%s/Partition3/data/lang", g_root));
        CHECK(exists("%s/Partition3/data/lang/text.big", g_root));
        printf("%s a drive's folders and files come back\n", g_fail ? "FAIL" : "PASS");
    }
    /* a folder named out of the partition's folder */
    {
        const int before = g_fail;
        const char *const up[] = {"../escape_dir"};
        CHECK(!load(up, 1, NULL));
        CHECK(!exists("%s/escape_dir", g_root));
        const char *const deep[] = {"data", "data/../../escape_dir2"};
        CHECK(!load(deep, 2, NULL));
        CHECK(!exists("%s/escape_dir2", g_root));
        const char *const back[] = {"..\\escape_dir3"};
        CHECK(!load(back, 1, NULL));
        CHECK(!exists("%s/escape_dir3", g_root));
        printf("%s a folder outside the partition's is refused\n", g_fail == before ? "PASS" : "FAIL");
    }
    /* a file named out of it: the store has its content, and it was written there */
    {
        const int before = g_fail;
        CHECK(!load(NULL, 0, "../escape.bin"));
        CHECK(!exists("%s/escape.bin", g_root));
        CHECK(!load(NULL, 0, "data/../../../escape2.bin"));
        CHECK(!exists("%s/escape2.bin", top));
        printf("%s a file outside the partition's is refused\n", g_fail == before ? "PASS" : "FAIL");
    }
    /* paths that are no path inside a folder: rooted, a drive's, empty parts, names FATX's can't be here */
    {
        const int before = g_fail;
        static const char *const bad[] = {"/rooted", "\\rooted", "C:/drive", "a//b", "a/", "", ".", "a/./b",
                                          "ends.in.dot.", "ends in space ", "stream:name"};
        for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            const char *const one[] = {bad[i]};
            if (load(one, 1, NULL)) {
                printf("FAIL the folder \"%s\" was taken\n", bad[i]);
                g_fail++;
            }
        }
        printf("%s what is no path inside a folder is refused\n", g_fail == before ? "PASS" : "FAIL");
    }
    /* counts the section can't hold: refused at once, nothing that size allocated or read */
    {
        const int before = g_fail;
        g_len = 0;
        put32(1);
        put32(3);
        put32(0);
        put32(0x01000000u);             /* 16 million folders in a section of 20 bytes */
        put32(0);
        NcRd r = {g_sec, g_len, 0, false};
        CHECK(!nc_cache_state_load(&r, NC_STATE_V_CACHE));
        g_len = 0;
        put32(1);
        put32(3);
        put32(0);
        put32(0);
        put32(0xFFFFFFFFu);
        NcRd r2 = {g_sec, g_len, 0, false};
        CHECK(!nc_cache_state_load(&r2, NC_STATE_V_CACHE));
        g_len = 0;
        put32(200);                     /* partitions past the six there are */
        NcRd r3 = {g_sec, g_len, 0, false};
        CHECK(!nc_cache_state_load(&r3, NC_STATE_V_CACHE));
        printf("%s counts the section can't hold are refused\n", g_fail == before ? "PASS" : "FAIL");
    }
    remove_tree(top);
    printf(g_fail ? "%d checks failed\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
