/*
 * Host test of a game folder read as a disc (native/core/fold_image.c) through
 * the disc reader itself (native/core/xdvdfs.c): a folder made by the test,
 * with what a game's has (a default.xbe, files of sizes around a sector's,
 * an empty one, subfolders, a folder of many names that differ by case and
 * length), opened by its default.xbe as the core opens it. Every file must
 * read back as it is on the host, by any spelling of its path, whole and in
 * parts across sector ends; the tables must hold each name once.
 *
 *   clang -O1 -g -fsanitize=address -I<repo> tests/native/disc/fold_test.c native/core/fold_image.c \
 *       native/core/xdvdfs.c -o fold_test
 */
#include "native/core/xdvdfs.h"
#include "native/core/chd_image.h"
#include "native/core/fold_image.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

/* what the two link to, not used here */
void nc_log(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}
bool nc_chd_attach(int fd) { (void)fd; return false; }
void nc_chd_detach(int fd) { (void)fd; }
bool nc_chd_is(int fd) { (void)fd; return false; }
bool nc_chd_read(int fd, void *buf, size_t len, uint64_t off) { (void)fd; (void)buf; (void)len; (void)off; return false; }

static int g_fail;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                       \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static char g_root[512];

/* The bytes of file number `seed`: its place in the file and the file both tell */
static uint8_t byte_at(unsigned seed, uint32_t i)
{
    return (uint8_t)(i * 31u + (i >> 8) * 7u + seed * 131u);
}

typedef struct TFile {
    const char *rel;        /* under the folder, '/' between names */
    uint32_t size;
} TFile;

/* default.xbe first; sizes around a sector (2048), an empty file, two levels of folders */
static const TFile k_files[] = {
    { "default.xbe", 5000 },
    { "empty.bin", 0 },
    { "one.bin", 1 },
    { "sector.bin", 2048 },
    { "sector_less.bin", 2047 },
    { "sector_more.bin", 2049 },
    { "media/big.dat", 70001 },
    { "media/Sound/track01.wma", 4096 },
    { "media/Sound/track02.wma", 3 },
    { "MAPS/a", 10 },
    { "MAPS/B", 11 },
    { "MAPS/c.map", 12 },
};
#define NFILES (sizeof(k_files) / sizeof(k_files[0]))
#define MANY 60             /* names in one folder: a table of more than a sector */

static void mkpath(char *out, size_t cap, const char *rel)
{
    snprintf(out, cap, "%s/%s", g_root, rel);
}

static bool put_file(const char *rel, unsigned seed, uint32_t size)
{
    char p[1024];
    mkpath(p, sizeof(p), rel);
    for (char *s = p + strlen(g_root) + 1; (s = strchr(s, '/')); s++) {     /* its folders */
        *s = 0;
        mkdir(p, 0777);
        *s = '/';
    }
    const int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY | O_BINARY, 0666);
    if (fd < 0) {
        return false;
    }
    uint8_t *b = malloc(size ? size : 1);
    for (uint32_t i = 0; i < size; i++) {
        b[i] = byte_at(seed, i);
    }
    if (!seed && size >= 4) {
        memcpy(b, "XBEH", 4);                       /* default.xbe is an XBE by its first word */
    }
    const bool ok = write(fd, b, size) == (ssize_t)size;
    free(b);
    close(fd);
    return ok;
}

static void many_name(unsigned i, char *out, size_t cap)
{
    /* lengths from 1 to ~40, cases mixed: the tables sort without case */
    static const char *const k_stem[] = { "a", "Zeta", "level", "TEXTURE_PACK", "x" };
    snprintf(out, cap, "%s%0*u%s", k_stem[i % 5], (int)(1 + i % 9), i, i % 3 ? ".bin" : "");
}

static bool check_file(const NcDisc *d, const char *path, unsigned seed, uint32_t size)
{
    NcDiscFile f;
    if (!nc_disc_find(d, path, &f)) {
        printf("  not found: %s\n", path);
        return false;
    }
    if (f.size != size || (f.attr & 0x10)) {
        printf("  %s: size %u for %u, attributes %02x\n", path, f.size, size, f.attr);
        return false;
    }
    uint8_t *b = malloc(size + 16);
    bool ok = !size || nc_disc_read(d, &f, 0, b, size);
    for (uint32_t i = 0; ok && i < size; i++) {
        const uint8_t want = !seed && i < 4 ? (uint8_t)"XBEH"[i] : byte_at(seed, i);
        if (b[i] != want) {
            printf("  %s: byte %u is %02x for %02x\n", path, i, b[i], want);
            ok = false;
        }
    }
    /* in parts that start and end off a sector's edge */
    for (uint32_t off = 1; ok && off < size; off += 1999) {
        const uint32_t n = size - off < 2500 ? size - off : 2500;
        ok = nc_disc_read(d, &f, off, b, n);
        for (uint32_t i = 0; ok && i < n; i++) {
            const uint8_t want = !seed && off + i < 4 ? (uint8_t)"XBEH"[off + i] : byte_at(seed, off + i);
            ok = b[i] == want;
        }
        if (!ok) {
            printf("  %s: a read of %u at %u differs\n", path, n, off);
        }
    }
    free(b);
    return ok;
}

typedef struct Names {
    unsigned n, files, dirs;
    char seen[MANY + 16][64];
    bool twice;
} Names;

static bool each_name(void *ctx, const uint8_t *e)
{
    Names *s = ctx;
    char name[64];
    const unsigned len = e[13] < 63 ? e[13] : 63;
    memcpy(name, e + 14, len);
    name[len] = 0;
    for (unsigned i = 0; i < s->n; i++) {
        s->twice |= !strcmp(s->seen[i], name);
    }
    if (s->n < MANY + 16) {
        strcpy(s->seen[s->n], name);
    }
    s->n++;
    if (e[12] & 0x10) {
        s->dirs++;
    } else {
        s->files++;
    }
    return false;
}

static int cmp_nocase(const uint8_t *a, unsigned an, const uint8_t *b, unsigned bn)
{
    for (unsigned i = 0; i < an && i < bn; i++) {
        const int x = toupper(a[i]), y = toupper(b[i]);
        if (x != y) {
            return x - y;
        }
    }
    return (int)an - (int)bn;
}

/* The table is the search tree a disc's is: under the entry at off, every name of its left
 * branch sorts before its own and every one of its right branch after, without case; lo and
 * hi (entries, NULL none) bound the branch. Returns the entries reached, 0 for a fault. */
static unsigned check_tree(const uint8_t *t, uint32_t size, uint32_t off, const uint8_t *lo, const uint8_t *hi,
                           unsigned depth)
{
    if (off + 14 > size || depth > 64) {
        return 0;
    }
    const uint8_t *e = t + off;
    const unsigned l = e[0] | e[1] << 8, r = e[2] | e[3] << 8;
    if (off + 14 + e[13] > size || (lo && cmp_nocase(lo + 14, lo[13], e + 14, e[13]) >= 0) ||
        (hi && cmp_nocase(e + 14, e[13], hi + 14, hi[13]) >= 0)) {
        return 0;
    }
    unsigned n = 1;
    if (l) {
        const unsigned k = check_tree(t, size, l * 4, lo, e, depth + 1);
        if (!k) {
            return 0;
        }
        n += k;
    }
    if (r) {
        const unsigned k = check_tree(t, size, r * 4, e, hi, depth + 1);
        if (!k) {
            return 0;
        }
        n += k;
    }
    return n;
}

/* A folder's table, each of its names once */
static void check_table(const NcDisc *d, const char *path, unsigned files, unsigned dirs)
{
    NcDiscFile f;
    CHECK(nc_disc_find(d, path, &f));
    CHECK(f.attr & 0x10);
    uint8_t *t = malloc(f.size ? f.size : 1);
    CHECK(nc_disc_read(d, &f, 0, t, f.size));
    Names s;
    memset(&s, 0, sizeof(s));
    nc_disc_dir_each(t, f.size, each_name, &s);
    CHECK(!s.twice);
    CHECK(s.files == files);
    CHECK(s.dirs == dirs);
    CHECK(check_tree(t, f.size, 0, NULL, NULL, 0) == files + dirs);    /* all of them, in order */
    free(t);
}

int main(void)
{
    const char *tmp = getenv("TMP");
    snprintf(g_root, sizeof(g_root), "%s/fold_test_%d", tmp && tmp[0] ? tmp : ".", (int)getpid());
    mkdir(g_root, 0777);
    for (unsigned i = 0; i < NFILES; i++) {
        CHECK(put_file(k_files[i].rel, i, k_files[i].size));
    }
    for (unsigned i = 0; i < MANY; i++) {
        char rel[128], name[64];
        many_name(i, name, sizeof(name));
        snprintf(rel, sizeof(rel), "many/%s", name);
        CHECK(put_file(rel, 100 + i, 17 * i));
    }

    /* the path a front end hands over: the folder, or its default.xbe */
    char xbe[1024], got[1024] = "untouched";
    mkpath(xbe, sizeof(xbe), "default.xbe");
    CHECK(nc_fold_xbe_path(g_root, got, sizeof(got)));
    CHECK(strlen(got) > 11 && !strcmp(got + strlen(got) - 11, "default.xbe"));
    char other[1024];
    mkpath(other, sizeof(other), "sector.bin");
    strcpy(got, "untouched");
    CHECK(!nc_fold_xbe_path(other, got, sizeof(got)));  /* a file that is no default.xbe */
    CHECK(!strcmp(got, "untouched"));
    char none[1024];
    mkpath(none, sizeof(none), "no_such_folder");
    CHECK(!nc_fold_xbe_path(none, got, sizeof(got)));

    /* a file that is no XBE is no folder */
    int ofd = open(other, O_RDONLY | O_BINARY);
    CHECK(ofd >= 0);
    CHECK(!nc_fold_attach(ofd));
    CHECK(!nc_fold_is(ofd));
    close(ofd);

    const int fd = open(xbe, O_RDONLY | O_BINARY);
    CHECK(fd >= 0);
    NcDisc d;
    CHECK(nc_disc_open(&d, fd));
    CHECK(nc_fold_is(fd));
    if (!g_fail) {
        for (unsigned i = 0; i < NFILES; i++) {
            CHECK(check_file(&d, k_files[i].rel, i, k_files[i].size));
        }
        for (unsigned i = 0; i < MANY; i++) {
            char rel[128], name[64];
            many_name(i, name, sizeof(name));
            snprintf(rel, sizeof(rel), "many/%s", name);
            CHECK(check_file(&d, rel, 100 + i, 17 * i));
        }
        /* either slash, a leading one, any case */
        CHECK(check_file(&d, "\\DEFAULT.XBE", 0, 5000));
        CHECK(check_file(&d, "\\Media\\sound\\TRACK01.WMA", 7, 4096));
        CHECK(check_file(&d, "maps/b", 10, 11));
        NcDiscFile f;
        CHECK(!nc_disc_find(&d, "media/missing.dat", &f));
        CHECK(!nc_disc_find(&d, "missing/big.dat", &f));
        CHECK(!nc_disc_find(&d, "media/big.dat/x", &f));    /* a file is no folder */
        /* the folders, each name once */
        check_table(&d, "media", 1, 1);
        check_table(&d, "media/Sound", 2, 0);
        check_table(&d, "maps", 3, 0);
        check_table(&d, "many", MANY, 0);
        /* the image past its files: zeros, not an error */
        uint8_t z[64];
        memset(z, 0xAA, sizeof(z));
        CHECK(nc_fold_read(fd, z, sizeof(z), 1ull << 40));
        for (unsigned i = 0; i < sizeof(z); i++) {
            CHECK(z[i] == 0);
        }
        /* the descriptor at sector 32, as a disc's */
        uint8_t vd[2048];
        CHECK(nc_fold_read(fd, vd, sizeof(vd), 32ull * 2048));
        CHECK(!memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20));
        CHECK(!memcmp(vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20));
    }
    nc_disc_close(&d);
    CHECK(!nc_fold_is(fd));
    close(fd);

    /* the test's folder goes */
    for (unsigned i = 0; i < MANY; i++) {
        char rel[128], name[64], p[1024];
        many_name(i, name, sizeof(name));
        snprintf(rel, sizeof(rel), "many/%s", name);
        mkpath(p, sizeof(p), rel);
        unlink(p);
    }
    for (unsigned i = 0; i < NFILES; i++) {
        char p[1024];
        mkpath(p, sizeof(p), k_files[i].rel);
        unlink(p);
    }
    static const char *const k_dirs[] = { "many", "media/Sound", "media", "MAPS", "" };
    for (unsigned i = 0; i < 5; i++) {
        char p[1024];
        mkpath(p, sizeof(p), k_dirs[i]);
        rmdir(p);
    }
    printf(g_fail ? "fold_test: %d failed\n" : "fold_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
