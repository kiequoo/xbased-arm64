/*
 * Host test of the kernel's files and devices (native/kernel/io.c) with no
 * title: the exports are called as a title's XAPI calls them, on a guest
 * memory and a hard disk folder of the test's own, under the address
 * sanitizer. What a title's paths, partitions and handles may do to the
 * emulator itself is what is looked at: nothing outside the hard disk
 * folder, nothing past a table or a buffer of the kernel's.
 *
 *   clang -O1 -g -fsanitize=address -DNC_DESKTOP=1 -I<repo> tests/native/kernel/io_test.c \
 *       native/kernel/io.c native/kernel/ob.c native/kernel/pool.c native/core/xdvdfs.c \
 *       native/state/state.c -lz -lpthread -o io_test
 */
#include "native/kernel/kernel.h"
#include "native/core/irq.h"
#include "native/core/xdvdfs.h"
#include "native/state/sections.h"
#include "native/state/state.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- the kernel around io.c, as far as it is called ---- */

struct NcFrame {
    uint32_t r[8];
};
uint32_t nc_frame_gpr(const NcFrame *f, int reg) { return f->r[reg]; }

static struct {
    const char *name;
    NcKFn fn;
} g_exports[96];
static unsigned g_nexports;

void nc_kernel_impl(NcKernel *k, const char *name, NcKFn fn)
{
    (void)k;
    if (g_nexports < sizeof(g_exports) / sizeof(g_exports[0])) {
        g_exports[g_nexports].name = name;
        g_exports[g_nexports++].fn = fn;
    }
}

static bool g_work;
void nc_irq_post_work(void) { __atomic_store_n(&g_work, true, __ATOMIC_RELEASE); }
bool nc_irq_take_work(void) { return __atomic_exchange_n(&g_work, false, __ATOMIC_ACQ_REL); }
bool nc_apc_queue(NcKernel *k, NcThread *t, const NcApc *a) { (void)k; (void)t; (void)a; return true; }
void nc_ke_block(NcKernel *k) { (void)k; }
void nc_ke_clear_event(NcKernel *k, uint32_t event) { (void)k; (void)event; }
void nc_ke_init_event(NcKernel *k, uint32_t event, unsigned type, bool signaled) { (void)k; (void)event; (void)type; (void)signaled; }
uint32_t nc_ke_set_event(NcKernel *k, uint32_t event) { (void)k; (void)event; return 0; }
void nc_ke_wake(NcKernel *k, NcThread *t, uint32_t status) { (void)k; (void)t; (void)status; }
void nc_ke_port_queue(NcKernel *k, uint32_t port, uint32_t key, uint32_t ctx, uint32_t status, uint32_t info)
{
    (void)k; (void)port; (void)key; (void)ctx; (void)status; (void)info;
}
uint64_t nc_ke_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void nc_kernel_frame_chain(NcKernel *k, NcFrame *f, char *buf, size_t len)
{
    (void)k; (void)f;
    if (len) {
        buf[0] = 0;
    }
}
bool nc_kernel_resident(NcKernel *k, uint32_t va, uint32_t len, bool write) { (void)k; (void)va; (void)len; (void)write; return true; }
void nc_kernel_trace_trigger(const char *what) { (void)what; }
bool nc_xc_security_sector(NcKernel *k, uint8_t *d, uint32_t n) { (void)k; (void)d; (void)n; return false; }
void nc_vm_pin(NcVm *vm, uint32_t pa, bool known, int lock) { (void)vm; (void)pa; (void)known; (void)lock; }
uint32_t nc_vm_alloc_phys_high(NcVm *vm, uint32_t len) { (void)vm; (void)len; return UINT32_MAX; }
/* the disc's other image formats (xdvdfs.c): none here */
bool nc_chd_attach(int fd) { (void)fd; return false; }
void nc_chd_detach(int fd) { (void)fd; }
bool nc_chd_is(int fd) { (void)fd; return false; }
bool nc_chd_read(int fd, void *buf, size_t len, uint64_t off) { (void)fd; (void)buf; (void)len; (void)off; return false; }
bool nc_fold_attach(int fd) { (void)fd; return false; }
void nc_fold_detach(int fd) { (void)fd; }
bool nc_fold_is(int fd) { (void)fd; return false; }
bool nc_fold_read(int fd, void *buf, size_t len, uint64_t off) { (void)fd; (void)buf; (void)len; (void)off; return false; }
NcVmDma nc_vm_dma;
void (*nc_vm_write_hook)(uint32_t va, uint32_t len);
void (*nc_vm_read_hook)(uint32_t va, uint32_t len);
static char g_last_log[512];
void nc_log(const char *tag, const char *fmt, ...)
{
    (void)tag;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_log, sizeof(g_last_log), fmt, ap);
    va_end(ap);
}

/* ---- the machine: 4 MiB of RAM at the contiguous window (0x80000000 up),
 * where the kernel keeps its own (the named objects' list at 0x8001A400);
 * the calls' arguments, a buffer and the pool above that ---- */

#define MEM         (4u << 20)
#define RAM_VA      0x80000000u
#define SCRATCH     (RAM_VA + 0x40000u)
#define SCRATCH_END (RAM_VA + 0x80000u)
#define BUF         (RAM_VA + 0x80000u)
#define POOL        (RAM_VA + 0x100000u)
static uint8_t *g_ram;                  /* the RAM itself */
static uint8_t *g_mem;                  /* guest address 0 (g_ram - RAM_VA): g_mem + va is a guest address's byte */
static uint32_t *g_pte;
static NcVm g_vm;
static NcKernel g_k;
static NcFrame g_f;
static NcDisc g_disc;
struct NcKernel *nc_state_kernel(void) { return &g_k; }

static int g_fail;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                       \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static uint32_t rd(uint32_t va) { return nc_vm_rd32(&g_vm, va); }
static void wr(uint32_t va, uint32_t v) { nc_vm_wr32(&g_vm, va, v); }

static uint64_t call(const char *name, const uint32_t *a)
{
    for (unsigned i = 0; i < g_nexports; i++) {
        if (!strcmp(g_exports[i].name, name)) {
            return g_exports[i].fn(&g_k, &g_f, a);
        }
    }
    printf("FAIL no export %s\n", name);
    exit(2);
}

/* a call's arguments in guest memory: the area starts again at each call */
static uint32_t g_at = SCRATCH;
static uint32_t take(uint32_t n)
{
    const uint32_t va = g_at;
    g_at += (n + 3) & ~3u;
    if (g_at > SCRATCH_END) {
        printf("FAIL scratch full\n");
        exit(2);
    }
    memset(g_mem + va, 0, n);
    return va;
}

/* an ANSI_STRING { Length, MaximumLength, Buffer } of s */
static uint32_t astr(const char *s)
{
    const uint32_t len = (uint32_t)strlen(s), buf = take(len + 1), va = take(8);
    memcpy(g_mem + buf, s, len + 1);
    *(uint16_t *)(g_mem + va) = (uint16_t)len;
    *(uint16_t *)(g_mem + va + 2) = (uint16_t)(len + 1);
    wr(va + 4, buf);
    return va;
}

/* OBJECT_ATTRIBUTES { RootDirectory, ObjectName, Attributes } */
static uint32_t oattr(const char *path)
{
    const uint32_t name = astr(path), va = take(12);
    wr(va + 4, name);
    wr(va + 8, 0x40);
    return va;
}

enum {
    FILE_OPEN = 1, FILE_CREATE = 2, FILE_OPEN_IF = 3,
    OPT_DIR = 0x01, SYNC = 0x20, NOT_DIR = 0x40,
    GENERIC_READ = (int)0x80000000u, GENERIC_WRITE = 0x40000000, DELETE = 0x00010000,
};
#define ST_OK 0u

static uint32_t nt_create(const char *path, uint32_t access, uint32_t share, uint32_t disp, uint32_t options,
                          uint32_t *handle)
{
    g_at = SCRATCH;
    const uint32_t h = take(4), iosb = take(8);
    const uint32_t a[9] = {h, access, oattr(path), iosb, 0, 0x80, share, disp, options};
    const uint32_t st = (uint32_t)call("NtCreateFile", a);
    *handle = st == ST_OK ? rd(h) : 0;
    return st;
}

static uint32_t nt_close(uint32_t handle)
{
    const uint32_t a[1] = {handle};
    return (uint32_t)call("NtClose", a);
}

/* NtWriteFile / NtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset) */
static uint32_t nt_io(const char *which, uint32_t handle, uint32_t buf, uint32_t len, int64_t off, uint32_t *done)
{
    g_at = SCRATCH;
    const uint32_t iosb = take(8), o = take(8);
    memcpy(g_mem + o, &off, 8);
    const uint32_t a[8] = {handle, 0, 0, 0, iosb, buf, len, off >= 0 ? o : 0};
    const uint32_t st = (uint32_t)call(which, a);
    if (done) {
        *done = rd(iosb + 4);
    }
    return st;
}

static uint32_t nt_delete(const char *path)
{
    g_at = SCRATCH;
    const uint32_t a[1] = {oattr(path)};
    return (uint32_t)call("NtDeleteFile", a);
}

static char g_root[256];

static bool exists(const char *fmt, ...)
{
    char p[1400];
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
        char p[1400];
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

#define P1 "\\Device\\Harddisk0\\Partition1"

/* ---- the cases ---- */

static void test_files(void)
{
    const int before = g_fail;
    uint32_t h, n = 0;
    CHECK(nt_create(P1 "\\saves", GENERIC_READ, 3, FILE_CREATE, OPT_DIR | SYNC, &h) == ST_OK);
    CHECK(nt_close(h) == ST_OK);
    CHECK(nt_create(P1 "\\saves\\a.sav", GENERIC_WRITE, 0, FILE_CREATE, SYNC | NOT_DIR, &h) == ST_OK);
    memcpy(g_mem + BUF, "hello, xbox", 11);
    CHECK(nt_io("NtWriteFile", h, BUF, 11, 0, &n) == ST_OK && n == 11);
    CHECK(nt_close(h) == ST_OK);
    CHECK(exists("%s/Partition1/saves/a.sav", g_root));
    /* the name in another case, as FATX finds it */
    CHECK(nt_create(P1 "\\SAVES\\A.SAV", GENERIC_READ, 1, FILE_OPEN, SYNC | NOT_DIR, &h) == ST_OK);
    memset(g_mem + BUF, 0, 64);
    CHECK(nt_io("NtReadFile", h, BUF, 64, 0, &n) == ST_OK && n == 11 && !memcmp(g_mem + BUF, "hello, xbox", 11));
    /* a second handle that wants to write while the first shares reading only */
    uint32_t h2;
    CHECK(nt_create(P1 "\\saves\\a.sav", GENERIC_WRITE, 3, FILE_OPEN, SYNC | NOT_DIR, &h2) == 0xC0000043u);
    CHECK(nt_close(h) == ST_OK);
    CHECK(nt_create(P1 "\\saves\\none.sav", GENERIC_READ, 1, FILE_OPEN, SYNC, &h) == 0xC0000034u);
    CHECK(nt_create(P1 "\\nowhere\\none.sav", GENERIC_READ, 1, FILE_OPEN, SYNC, &h) == 0xC000003Au);
    printf("%s files are made, written, read and shared\n", g_fail == before ? "PASS" : "FAIL");
}

static void test_dotdot(void)
{
    const int before = g_fail;
    uint32_t h;
    /* ".." folds inside the object namespace: past a partition it names no device */
    CHECK(nt_create(P1 "\\..\\..\\..\\io_escape.txt", GENERIC_WRITE, 0, FILE_OPEN_IF, SYNC, &h) != ST_OK);
    CHECK(nt_create(P1 "\\saves\\..\\..\\..\\..\\..\\io_escape.txt", GENERIC_WRITE, 0, FILE_OPEN_IF, SYNC, &h) != ST_OK);
    g_at = SCRATCH;
    const uint32_t link[2] = {astr("\\??\\E:"), astr(P1)};
    CHECK((uint32_t)call("IoCreateSymbolicLink", link) == ST_OK);
    CHECK(nt_create("\\??\\E:\\..\\..\\..\\io_escape.txt", GENERIC_WRITE, 0, FILE_OPEN_IF, SYNC, &h) != ST_OK);
    CHECK(nt_create("\\??\\E:/../../../io_escape.txt", GENERIC_WRITE, 0, FILE_OPEN_IF, SYNC, &h) != ST_OK);
    CHECK(!exists("%s/io_escape.txt", g_root) && !exists("%s/../io_escape.txt", g_root) &&
          !exists("%s/../../io_escape.txt", g_root));
    /* and to another partition it is that partition's file */
    CHECK(nt_create("\\??\\E:\\saves\\..\\..\\Partition2\\b.txt", GENERIC_WRITE, 0, FILE_OPEN_IF, SYNC, &h) == ST_OK);
    CHECK(nt_close(h) == ST_OK);
    CHECK(exists("%s/Partition2/b.txt", g_root));
    printf("%s \"..\" never leaves the hard disk folder\n", g_fail == before ? "PASS" : "FAIL");
}

/* A cache drive's format writes its FATX superblock to the partition raw:
 * the cluster size is kept per partition, for any partition a path can name. */
static void test_partitions(void)
{
    const int before = g_fail;
    static const char *const parts[] = {"\\Device\\Harddisk0\\Partition5", "\\Device\\Harddisk0\\Partition6",
                                        "\\Device\\Harddisk0\\Partition7", "\\Device\\Harddisk0\\Partition9"};
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        uint32_t h, n = 0;
        CHECK(nt_create(parts[i], GENERIC_READ | GENERIC_WRITE, 3, FILE_OPEN, SYNC, &h) == ST_OK);
        memset(g_mem + BUF, 0, 4096);
        memcpy(g_mem + BUF, "FATX", 4);
        wr(BUF + 4, 0x12345678);        /* the volume's id */
        wr(BUF + 8, 32);                /* sectors a cluster */
        CHECK(nt_io("NtWriteFile", h, BUF, 4096, 0, &n) == ST_OK && n == 4096);
        CHECK(nt_close(h) == ST_OK);
    }
    printf("%s a superblock written to any partition stays in the kernel's tables\n", g_fail == before ? "PASS" : "FAIL");
}

/* Partition0 is the disk raw: no file system to rename into or delete from. */
static void test_partition0(void)
{
    const int before = g_fail;
    uint32_t h;
    CHECK(nt_delete("\\Device\\Harddisk0\\Partition0\\x.bin") != ST_OK);
    CHECK(nt_create(P1 "\\saves\\move.sav", GENERIC_WRITE | DELETE, 0, FILE_OPEN_IF, SYNC | NOT_DIR, &h) == ST_OK);
    /* FILE_RENAME_INFORMATION { ReplaceIfExists, RootDirectory, FileName (an ANSI_STRING) } */
    g_at = SCRATCH;
    const uint32_t name = astr("\\Device\\Harddisk0\\Partition0\\moved.sav"), info = take(16), iosb = take(8);
    memcpy(g_mem + info + 8, g_mem + name, 8);
    const uint32_t a[5] = {h, iosb, info, 16, 10};
    CHECK((uint32_t)call("NtSetInformationFile", a) != ST_OK);
    CHECK(nt_close(h) == ST_OK);
    CHECK(exists("%s/Partition1/saves/move.sav", g_root) && !exists("%s/Partition0", g_root));
    printf("%s nothing is renamed into or deleted from the raw disk\n", g_fail == before ? "PASS" : "FAIL");
}

/* An open that gets no handle (the table full) isn't open: its file's share
 * isn't held by nobody afterwards. */
static void test_handles(void)
{
    const int before = g_fail;
    static uint32_t held[NC_MAX_HANDLES];
    unsigned n = 0;
    uint32_t h, st = ST_OK;
    while (n < NC_MAX_HANDLES - 1 && (st = nt_create(P1, GENERIC_READ, 3, FILE_OPEN, OPT_DIR | SYNC, &h)) == ST_OK) {
        held[n++] = h;
    }
    /* every handle there is (the table's slots but the first) is taken, and
     * no more were asked for: the open that finds none left is the file's */
    CHECK(st == ST_OK && n == NC_MAX_HANDLES - 1);
    CHECK(nt_create(P1 "\\saves\\only.sav", GENERIC_WRITE, 0, FILE_OPEN_IF, SYNC | NOT_DIR, &h) ==
          NC_STATUS_INSUFFICIENT_RESOURCES);
    CHECK(n && nt_close(held[--n]) == ST_OK);
    CHECK(nt_create(P1 "\\saves\\only.sav", GENERIC_WRITE, 0, FILE_OPEN_IF, SYNC | NOT_DIR, &h) == ST_OK);
    CHECK(nt_close(h) == ST_OK);
    while (n) {
        nt_close(held[--n]);
    }
    printf("%s an open that gets no handle leaves nothing open\n", g_fail == before ? "PASS" : "FAIL");
}

/* A path through folders deep enough that the host's path is longer than
 * the kernel's buffers: no such path, and no byte past the buffer. Made on
 * the host directly (through the kernel a path that long can't be made);
 * skipped where the host can't hold one. */
static void test_long_path(void)
{
    const int before = g_fail;
    char host[1400], guest[600] = P1;
    int hl = snprintf(host, sizeof(host), "%s/Partition1", g_root);
    bool made = true;
    for (int i = 0; made && i < 4; i++) {
        char name[116];
        memset(name, 'a' + i, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        hl += snprintf(host + hl, sizeof(host) - (size_t)hl, "/%s", name);
        made = mkdir(host, 0775) == 0;
        snprintf(guest + strlen(guest), sizeof(guest) - strlen(guest), "\\%s", name);
    }
    if (!made || hl <= 512) {
        printf("skip a host path longer than the kernel's buffers (%s)\n",
               made ? "the folder's own path is too short for it" : "this host can't hold one");
        return;
    }
    uint32_t h;
    char path[700];
    snprintf(path, sizeof(path), "%s\\q\\r", guest);
    CHECK(nt_create(path, GENERIC_READ, 3, FILE_OPEN, SYNC, &h) != ST_OK);
    snprintf(path, sizeof(path), "%s\\q", guest);
    CHECK(nt_create(path, GENERIC_WRITE, 3, FILE_OPEN_IF, SYNC, &h) != ST_OK);
    CHECK(nt_delete(path) != ST_OK);
    printf("%s a host path longer than the kernel's buffers is no path\n", g_fail == before ? "PASS" : "FAIL");
}

/* The open files in a savestate: a state names each by its place in the
 * hard disk folder, and one that names a place outside is refused. Last:
 * a refused load leaves no file open. */
static void test_state(void)
{
    const int before = g_fail;
    uint32_t h;
    CHECK(nt_create(P1 "\\saves\\state.sav", GENERIC_READ | GENERIC_WRITE, 3, FILE_OPEN_IF, SYNC | NOT_DIR, &h) == ST_OK);
    NcBuf b = {0};
    CHECK(nc_file_state_save(&b) && !b.fail);
    NcRd r = {b.p, b.len, 0, false};
    CHECK(nc_file_state_load(&r, NC_STATE_V_FILE));
    static const char was[] = "Partition1/saves/state.sav";
    static const char *const bad[] = {"../io_escape_state.sav", "Partition1/../../io_escape_state.sav",
                                      "/io_escape_state.sav", "C:\\io_escape_state.sav"};
    uint8_t *at = NULL;
    for (size_t i = 0; b.p && i + sizeof(was) <= b.len && !at; i++) {
        at = !memcmp(b.p + i, was, sizeof(was)) ? b.p + i : NULL;
    }
    CHECK(at);
    for (unsigned i = 0; at && i < sizeof(bad) / sizeof(bad[0]); i++) {
        memset(at, 0, 64);
        memcpy(at, bad[i], strlen(bad[i]));
        NcRd r2 = {b.p, b.len, 0, false};
        if (nc_file_state_load(&r2, NC_STATE_V_FILE)) {
            printf("FAIL a state's file at \"%s\" was opened\n", bad[i]);
            g_fail++;
        }
    }
    nc_buf_free(&b);
    printf("%s a state's file outside the hard disk folder is refused\n", g_fail == before ? "PASS" : "FAIL");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* a line is out before the sanitizer may stop the program */
    const char *tmp = getenv("TMP");
    char top[200];
    snprintf(top, sizeof(top), "%s/io_test_%d", tmp && tmp[0] ? tmp : ".", (int)getpid());
    snprintf(g_root, sizeof(g_root), "%s/a_hard_disk_folder_with_a_name_long_enough_for_the_path_test", top);
    mkdir(top, 0775);
    mkdir(g_root, 0775);

    g_ram = calloc(1, MEM);
    g_pte = calloc((size_t)1 << 20, sizeof(uint32_t));
    if (!g_ram || !g_pte) {
        return 2;
    }
    g_mem = (uint8_t *)((uintptr_t)g_ram - RAM_VA);
    for (uint32_t i = 0; i < MEM / NC_PAGE; i++) {
        g_pte[RAM_VA / NC_PAGE + i] = (i * NC_PAGE) | NC_PTE_VALID;
    }
    g_vm.base = g_mem;
    g_vm.phys = g_ram;
    g_vm.pte = g_pte;
    g_vm.ram_size = MEM;
    g_k.vm = &g_vm;
    g_disc.fd = -1;
    g_k.disc = &g_disc;
    for (unsigned t = 0; t < NC_OB_TYPES; t++) {
        g_k.type_va[t] = 0x8000 + 16 * t;
    }
    nc_pool_init(&g_k, POOL, RAM_VA + MEM - POOL);
    nc_io_set_hdd_root(g_root);
    nc_ob_register(&g_k);
    nc_io_register(&g_k);

    test_files();
    test_dotdot();
    test_partitions();
    test_partition0();
    test_handles();
    test_long_path();
    test_state();

    usleep(300000);                     /* the files closed on their thread */
    remove_tree(top);
    printf(g_fail ? "%d checks failed\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
