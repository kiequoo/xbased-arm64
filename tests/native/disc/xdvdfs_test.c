/*
 * Host test of the XDVDFS reader's directory walk (native/core/xdvdfs.c,
 * nc_disc_dir_each) on tables as a disc has them and as a damaged or crafted
 * image may: a list-shaped tree, links that loop, entries that overlap.
 * A disc image comes from outside: no table may make the walk write past its
 * own memory or run for ever. Built with the address sanitizer where the
 * compiler has it (tools/native/host-tests.sh), which is what catches a
 * write past the walk's stack.
 *
 *   clang -O1 -g -fsanitize=address -I<repo> tests/native/disc/xdvdfs_test.c native/core/xdvdfs.c -o xdvdfs_test
 */
#include "native/core/xdvdfs.h"
#include "native/core/chd_image.h"
#include "native/core/fold_image.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* what xdvdfs.c links to, not used by the walk */
void nc_log(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}
bool nc_chd_attach(int fd) { (void)fd; return false; }
void nc_chd_detach(int fd) { (void)fd; }
bool nc_chd_is(int fd) { (void)fd; return false; }
bool nc_chd_read(int fd, void *buf, size_t len, uint64_t off) { (void)fd; (void)buf; (void)len; (void)off; return false; }
bool nc_fold_attach(int fd) { (void)fd; return false; }
void nc_fold_detach(int fd) { (void)fd; }
bool nc_fold_is(int fd) { (void)fd; return false; }
bool nc_fold_read(int fd, void *buf, size_t len, uint64_t off) { (void)fd; (void)buf; (void)len; (void)off; return false; }

static int g_fail;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                       \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static void w16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* An entry at off: links in dwords, a one-letter name and its number. */
static uint32_t put(uint8_t *dir, uint32_t off, unsigned left, unsigned right, const char *name)
{
    const size_t n = strlen(name);
    w16(dir + off, left);
    w16(dir + off + 2, right);
    memset(dir + off + 4, 0, 8);
    dir[off + 12] = 0x20;
    dir[off + 13] = (uint8_t)n;
    memcpy(dir + off + 14, name, n);
    return (uint32_t)((off + 14 + n + 3) & ~3u);
}

typedef struct {
    unsigned n;
    char names[64][16];
} Seen;

static bool collect(void *ctx, const uint8_t *e)
{
    Seen *s = ctx;
    if (s->n < 64) {
        memcpy(s->names[s->n], e + 14, e[13] < 15 ? e[13] : 15);
        s->names[s->n][e[13] < 15 ? e[13] : 15] = 0;
    }
    s->n++;
    return false;
}

static bool count(void *ctx, const uint8_t *e)
{
    (void)e;
    ++*(unsigned *)ctx;
    return false;
}

/* a tree as a disc has it: B at the root, A to its left, C to its right */
static void test_tree(void)
{
    uint8_t dir[2048];
    memset(dir, 0xFF, sizeof(dir));
    uint32_t a = put(dir, 0, 0, 0, "B");
    uint32_t c = put(dir, a, 0, 0, "A");
    put(dir, c, 0, 0, "C");
    w16(dir + 0, a / 4);
    w16(dir + 2, c / 4);
    Seen s = {0};
    CHECK(!nc_disc_dir_each(dir, sizeof(dir), collect, &s));
    CHECK(s.n == 3);
    CHECK(!strcmp(s.names[0], "A") && !strcmp(s.names[1], "B") && !strcmp(s.names[2], "C"));
    printf("%s tree in order\n", s.n == 3 ? "PASS" : "FAIL");
}

/* a tree an image tool left a list: every entry the next one's left child */
static void test_list(void)
{
    enum { N = 300 };
    static uint8_t dir[N * 20 + 2048];
    memset(dir, 0xFF, sizeof(dir));
    uint32_t off = 0;
    for (unsigned i = 0; i < N; i++) {
        char name[8];
        snprintf(name, sizeof(name), "F%03u", i);
        /* entries don't cross a sector's end */
        if (2048 - off % 2048 < 20) {
            off = (off / 2048 + 1) * 2048;
        }
        const uint32_t next = put(dir, off, 0, 0, name);
        uint32_t at = next;
        if (2048 - at % 2048 < 20) {
            at = (at / 2048 + 1) * 2048;
        }
        if (i + 1 < N) {
            w16(dir + off, at / 4);
        }
        off = next;
    }
    unsigned n = 0;
    CHECK(!nc_disc_dir_each(dir, off, count, &n));
    CHECK(n == N);
    printf("%s a list-shaped tree, %u deep\n", n == N ? "PASS" : "FAIL", (unsigned)N);
}

/* links that loop: each entry once, and an end */
static void test_loop(void)
{
    uint8_t dir[2048];
    memset(dir, 0xFF, sizeof(dir));
    uint32_t b = put(dir, 0, 0, 0, "A");
    put(dir, b, 0, 0, "B");
    w16(dir + 0, b / 4);        /* A's left is B */
    w16(dir + b, 0);            /* B has none... */
    w16(dir + b + 2, b / 4);    /* ...and its right is itself */
    unsigned n = 0;
    CHECK(!nc_disc_dir_each(dir, sizeof(dir), count, &n));
    CHECK(n == 2);
    printf("%s links that loop\n", n == 2 ? "PASS" : "FAIL");
}

/* Entries that overlap, one every 4 bytes, each the next one's left child:
 * four times the entries a table of whole ones holds. The walk's stack was
 * sized for whole entries and this chain wrote past it. An entry's name
 * length (its byte 13) is the high byte of the left link three slots on, so
 * the chain starts where the links are 256 or more. */
static void test_overlap(void)
{
    enum { SIZE = 0x10000 };
    uint8_t *dir = calloc(1, SIZE);
    const unsigned first = 256, last = (SIZE - 14 - 255) / 4 - 4;
    for (unsigned s = first; s <= last + 3; s++) {
        w16(dir + 4 * s, s + 1);
    }
    w16(dir + 4 * last, 0);     /* the chain's end */
    w16(dir + 0, first);        /* the root's left: the chain */
    dir[13] = 1;                /* the root's own name length */
    unsigned n = 0;
    CHECK(!nc_disc_dir_each(dir, SIZE, count, &n));
    /* the chain's slots once each (but its last three, whose name length is the cut link's high byte):
     * more than three times the whole entries the table could hold */
    const unsigned whole = SIZE / 14;
    CHECK(n > 3 * whole && n <= SIZE / 4);
    printf("%s overlapping entries: %u seen where %u whole ones fit\n", n > 3 * whole && n <= SIZE / 4 ? "PASS" : "FAIL",
           n, whole);
    free(dir);
}

int main(void)
{
    test_tree();
    test_list();
    test_loop();
    test_overlap();
    printf(g_fail ? "%d checks failed\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
