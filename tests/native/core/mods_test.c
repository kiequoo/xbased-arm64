/*
 * Host test of the mods' manifest and of their savestate section
 * (native/core/mods.c) over a disc and a guest memory of the test's own: the
 * patches of a manifest go over the XBE as the core reads it, a state saved
 * with them loads again whatever a patch's length (up to a manifest line's),
 * a state made with other mods gets theirs taken back and the run's written,
 * and a section that is not what a save writes is refused.
 *
 *   clang -O1 -g -fsanitize=address -I<repo> tests/native/core/mods_test.c native/core/mods.c \
 *       native/core/cheats.c hw/xbox/hle/core/xbe.c native/state/state.c -lz -lpthread -o mods_test
 */
#include "native/core/cheats.h"
#include "native/core/mods.h"
#include "native/state/sections.h"
#include "native/state/state.h"
#include "tests/hle/xbe_fixture.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BASE    0x10000u
#define TEXT    0x11000u            /* the XBE's code: 0x2000 bytes at 0x1000 of its file */
#define DATA    0x13000u            /* a page the cheat writes */
#define FILE_SZ 0x3000u
#define BIG     337u                /* the longest pattern of the widescreen database */

static uint8_t g_orig[FILE_SZ];     /* the disc's XBE */
static uint8_t g_file[FILE_SZ];     /* ... as the core reads it: with the patches */
static uint8_t g_ram[0x4000];       /* guest pages from BASE */
static uint32_t g_pte[1u << 20];
static NcVm g_vm = {.pte = g_pte, .phys = g_ram, .ram_size = sizeof(g_ram)};
static unsigned g_host_writes, g_invalidated, g_overlaid;
static uint64_t g_vblanks;

/* ---- what mods.c asks the rest of the core for ---- */

void nc_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }

static bool is_xbe(const char *path)
{
    while (*path == '\\' || *path == '/') {
        path++;
    }
    return !strcmp(path, "default.xbe");
}

bool nc_disc_find(const NcDisc *d, const char *path, NcDiscFile *out)
{
    (void)d;
    out->sector = 1;
    out->size = FILE_SZ;
    out->attr = 0;
    return is_xbe(path);
}

bool nc_disc_find_orig(const NcDisc *d, const char *path, NcDiscFile *out) { return nc_disc_find(d, path, out); }

static bool file_read(const uint8_t *file, uint64_t off, void *buf, size_t len)
{
    if (off > FILE_SZ || len > FILE_SZ - off) {
        return false;
    }
    memcpy(buf, file + off, len);
    return true;
}

bool nc_disc_read(const NcDisc *d, const NcDiscFile *f, uint64_t off, void *buf, size_t len)
{
    (void)d; (void)f;
    return file_read(g_file, off, buf, len);
}

bool nc_disc_read_orig(const NcDisc *d, const NcDiscFile *f, uint64_t off, void *buf, size_t len)
{
    (void)d; (void)f;
    return file_read(g_orig, off, buf, len);
}

bool nc_disc_overlay_patch(NcDisc *d, const char *path, uint64_t off, const uint8_t *bytes, uint32_t len)
{
    (void)d;
    if (!is_xbe(path) || off > FILE_SZ || len > FILE_SZ - off) {
        return false;
    }
    memcpy(g_file + off, bytes, len);
    g_overlaid++;
    return true;
}

bool nc_disc_overlay_file(NcDisc *d, const char *path, const char *host) { (void)d; (void)path; (void)host; return false; }
void nc_disc_overlay_seal(NcDisc *d) { (void)d; }
bool nc_io_host_file(const char *xbox_path, char *out, size_t n) { (void)xbox_path; (void)out; (void)n; return false; }
uint32_t nc_mm_contig_free_callers(struct NcKernel *k, uint32_t lo, uint32_t hi) { (void)k; (void)lo; (void)hi; return 0; }
void nc_cpu_invalidate(struct NcCpu *cpu, uint32_t va, uint32_t len) { (void)cpu; (void)va; (void)len; g_invalidated++; }
void hle_host_mem_set_dirty(uint32_t pa, uint32_t len) { (void)pa; (void)len; g_host_writes++; }
uint64_t nc_dev_nv2a_vblanks(void) { return g_vblanks; }
void nc_dev_nv2a_vblank_multiply(unsigned n) { (void)n; }
void nc_memscan_poll(const NcVm *vm) { (void)vm; }

/* ---- the test ---- */

static int g_fail;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                       \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static void hex(FILE *fp, const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        fprintf(fp, "%02X ", b[i]);
    }
}

/* A MODS section by hand: the count, then each patch's record (the XBE's name in 128 bytes, the
 * VA, the length) and its two runs of bytes. */
static uint8_t g_sec[8192];
static size_t g_len;

static void sec_begin(uint32_t npatches)
{
    memset(g_sec, 0, sizeof(g_sec));
    memcpy(g_sec, &npatches, 4);
    g_len = 4;
}

static void sec_patch(const char *xbe, uint32_t va, uint32_t n, const uint8_t *old, const uint8_t *neu, uint32_t have)
{
    snprintf((char *)g_sec + g_len, 128, "%s", xbe);
    memcpy(g_sec + g_len + 128, &va, 4);
    memcpy(g_sec + g_len + 132, &n, 4);
    g_len += 136;
    memcpy(g_sec + g_len, old, have);
    memcpy(g_sec + g_len + have, neu, have);
    g_len += 2 * (size_t)have;
}

static bool sec_load(void)
{
    NcRd r = {g_sec, g_len, 0, false};
    return nc_mods_state_load(&r, NC_STATE_V_MODS);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* the disc's XBE: its headers and one executable section */
    static const FxSection text = {".text", 4, TEXT, 0x2000, 0x1000, 0x2000};
    fx_build_xbe(g_orig, 0x1000, TEXT + 0x100, false, &text, 1, NULL, 0, 0x4D540001);
    for (uint32_t i = 0x1000; i < FILE_SZ; i++) {
        g_orig[i] = (uint8_t)(i * 7 + 3);
    }
    memcpy(g_file, g_orig, FILE_SZ);
    HleXbeInfo x;
    CHECK(hle_xbe_parse(g_orig, 0x1000, &x) == HLE_XBE_OK);
    for (uint32_t i = 0; i < sizeof(g_ram) / NC_PAGE; i++) {
        g_pte[(BASE >> 12) + i] = i * NC_PAGE | NC_PTE_VALID;
    }

    /* the manifest: a patch as long as the database's longest, a short one with no bytes to
     * check, one of a mod that is off, a cheat with a line that is not a code */
    uint8_t big_old[BIG], big_new[BIG], off_old[300], off_new[300];
    for (unsigned i = 0; i < BIG; i++) {
        big_old[i] = g_orig[0x1010 + i];
        big_new[i] = (uint8_t)~big_old[i];
    }
    for (unsigned i = 0; i < sizeof(off_old); i++) {
        off_old[i] = g_orig[0x1800 + i];
        off_new[i] = (uint8_t)(off_old[i] + 1);
    }
    char manifest[600];
    const char *tmp = getenv("TMP");
    snprintf(manifest, sizeof(manifest), "%s/mods_test_%d.txt", tmp && tmp[0] ? tmp : ".", (int)getpid());
    FILE *fp = fopen(manifest, "w");
    CHECK(fp != NULL);
    if (!fp) {
        return 1;
    }
    fprintf(fp, "patch\tdefault.xbe\tbig\t0x%08X: ", TEXT + 0x10);
    hex(fp, big_old, BIG);
    fprintf(fp, "-> ");
    hex(fp, big_new, BIG);
    fprintf(fp, "\npatch\tdefault.xbe\tsmall\t0x%08X: 90 90\n", TEXT + 0x400);
    fprintf(fp, "patchoff\tdefault.xbe\toff\t0x%08X: ", TEXT + 0x800);
    hex(fp, off_old, sizeof(off_old));
    fprintf(fp, "-> ");
    hex(fp, off_new, sizeof(off_new));
    fprintf(fp, "\npatch\tdefault.xbe\twrong\t0x%08X: 01 02 -> 03 04\n", TEXT + 0xC00);   /* other bytes there */
    fprintf(fp, "patch\tdefault.xbe\toutside\t0x%08X: 90\n", TEXT + 0x2000);              /* past the section */
    fprintf(fp, "cheat\t1\t0\tlives\ncode\tw8 0x%08X 9\ncode\tend x\n", DATA);
    fclose(fp);
    nc_mods_set_manifest(manifest);
    nc_mods_install(NULL);
    unlink(manifest);
    CHECK(g_overlaid == 2);
    CHECK(!memcmp(g_file + 0x1010, big_new, BIG));
    CHECK(g_file[0x1010 + BIG] == g_orig[0x1010 + BIG]);
    CHECK(g_file[0x1400] == 0x90 && g_file[0x1401] == 0x90 && g_file[0x1402] == g_orig[0x1402]);
    CHECK(!memcmp(g_file + 0x1800, g_orig + 0x1800, sizeof(off_old)));     /* off: the disc's */
    CHECK(!memcmp(g_file + 0x1C00, g_orig + 0x1C00, 4));
    CHECK(nc_cheat_count() == 1);

    /* the title loaded: the file as the core reads it, in memory */
    memcpy(g_ram, g_file, FILE_SZ);
    nc_mods_bind(&g_vm, NULL, &x);
    g_vblanks = 1;
    nc_mods_poll();
    CHECK(g_ram[DATA - BASE] == 9);

    /* sections that are not what a save writes */
    const uint8_t two[2] = {0xCC, 0xCC};
    static uint8_t more[600];
    sec_begin(1);
    sec_patch("default.xbe", TEXT + 0x600, 513, more, more, 513);          /* longer than a line gives */
    CHECK(!sec_load());
    sec_begin(1);
    sec_patch("default.xbe", TEXT + 0x600, 0, more, more, 0);
    CHECK(!sec_load());
    sec_begin(0x10000000u);                                                 /* a count its bytes don't hold */
    CHECK(!sec_load());
    sec_begin(1);
    sec_patch("default.xbe", TEXT + 0x600, 300, more, more, 100);           /* cut short */
    CHECK(!sec_load());

    /* a state saved now loads again, the long patch with it; its memory already has the run's
     * patches: nothing is written */
    NcBuf b = {0};
    CHECK(nc_mods_state_save(&b) && !b.fail);
    CHECK(b.len == 4 + 2 * 136 + 2 * BIG + 2 * 2);
    NcRd r = {b.p, b.len, 0, false};
    CHECK(nc_mods_state_load(&r, NC_STATE_V_MODS));
    g_host_writes = 0;
    nc_mods_after_load(NULL, "\\default.xbe");
    CHECK(g_host_writes == 0);
    CHECK(!memcmp(g_ram, g_file, FILE_SZ));
    nc_buf_free(&b);

    /* a state made with another mod on and without the long patch: the other mod's bytes go back
     * to the disc's, the run's patches are written (the long one whole) and their code dropped */
    memcpy(g_ram + 0x1010, big_old, BIG);
    g_ram[0x1600] = g_ram[0x1601] = 0xCC;
    sec_begin(1);
    sec_patch("default.xbe", TEXT + 0x600, 2, g_orig + 0x1600, two, 2);
    CHECK(sec_load());
    g_invalidated = 0;
    nc_mods_after_load(NULL, "\\default.xbe");
    CHECK(!memcmp(g_ram + 0x1010, big_new, BIG));
    CHECK(g_ram[0x1010 + BIG] == g_orig[0x1010 + BIG]);
    CHECK(g_ram[0x1400] == 0x90 && g_ram[0x1401] == 0x90);
    CHECK(g_ram[0x1600] == g_orig[0x1600] && g_ram[0x1601] == g_orig[0x1601]);
    CHECK(g_invalidated > 0);
    CHECK(!memcmp(g_ram, g_file, FILE_SZ));

    printf(g_fail ? "%d checks failed\n" : "all passed\n", g_fail);
    return g_fail != 0;
}
