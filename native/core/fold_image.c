/*
 * hakuX native core — a game folder as a disc (fold_image.h).
 *
 * The image is laid out as an XDVDFS writer would: the volume descriptor at
 * sector 32, then each folder's directory table (a binary tree of its
 * names, case-insensitive, the root entry first; an entry never across a
 * sector's end), then the files, each from a sector of its own. The tables
 * and descriptor live in memory; a file's sectors are read from the file
 * (its descriptor opened when first read, a few kept open).
 */
#include "native/core/fold_image.h"
#include "native/core/nc.h"
#include "native/core/xdvdfs.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

#define MAX_FOLDS 4
#define FD_CACHE 8
#define SECTOR NC_XDVDFS_SECTOR

typedef struct Node {
    char name[256];
    char *host;                 /* the file's path (the file list takes it) */
    bool dir;
    uint64_t size;              /* a file's bytes, a folder's table's */
    uint32_t sector;
    struct Node *kids;
    unsigned nkids;
} Node;

typedef struct {
    char *host;
    uint32_t sector, size;
} FFile;

typedef struct {
    int fd;                     /* the default.xbe's: the key (-1: free) */
    uint8_t *meta;              /* sectors [0, meta_sectors): descriptor and tables */
    uint32_t meta_sectors;
    FFile *files;               /* by sector */
    unsigned nfiles;
    uint64_t size;
    pthread_mutex_t mu;
    int cache_fd[FD_CACHE];
    unsigned cache_file[FD_CACHE], next;
} Fold;

static Fold g_fold[MAX_FOLDS] = {{-1}, {-1}, {-1}, {-1}};
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static Fold *find(int fd)
{
    for (int i = 0; i < MAX_FOLDS; i++) {
        if (fd >= 0 && g_fold[i].fd == fd) {
            return &g_fold[i];
        }
    }
    return NULL;
}

static int name_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        const int x = toupper((unsigned char)*a), y = toupper((unsigned char)*b);
        if (x != y || !x) {
            return x - y;
        }
    }
}

static int node_cmp(const void *a, const void *b)
{
    return name_cmp(((const Node *)a)->name, ((const Node *)b)->name);
}

static void node_free(Node *n)
{
    for (unsigned i = 0; i < n->nkids; i++) {
        node_free(&n->kids[i]);
    }
    free(n->kids);
    free(n->host);
}

/* A folder's entries (files under 4 GiB, names up to 255 bytes), sorted;
 * its subfolders' too. */
static bool scan(Node *dir, const char *path, unsigned depth, unsigned *count)
{
    DIR *d = opendir(path);
    if (!d) {
        return false;
    }
    struct dirent *de;
    unsigned cap = 0;
    while ((de = readdir(d))) {
        const size_t nl = strlen(de->d_name);
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || nl > 255) {
            continue;
        }
        char p[1024];
        if ((size_t)snprintf(p, sizeof(p), "%s/%s", path, de->d_name) >= sizeof(p)) {
            continue;
        }
        struct stat st;
        if (stat(p, &st) || (!S_ISDIR(st.st_mode) && (uint64_t)st.st_size >= 0xFFFFFFFFull)) {
            continue;
        }
        if (dir->nkids == cap) {
            cap = cap ? cap * 2 : 16;
            Node *k = realloc(dir->kids, cap * sizeof(Node));
            if (!k) {
                break;
            }
            dir->kids = k;
        }
        Node *n = &dir->kids[dir->nkids++];
        memset(n, 0, sizeof(*n));
        memcpy(n->name, de->d_name, nl + 1);
        n->dir = S_ISDIR(st.st_mode);
        if (n->dir) {
            if (depth < 32) {
                scan(n, p, depth + 1, count);
            }
        } else {
            n->host = strdup(p);
            n->size = (uint64_t)st.st_size;
        }
        ++*count;
    }
    closedir(d);
    if (dir->nkids) {
        qsort(dir->kids, dir->nkids, sizeof(Node), node_cmp);
    }
    return true;
}

static uint32_t entry_len(const Node *n)
{
    return (14u + (uint32_t)strlen(n->name) + 3u) & ~3u;
}

/* The table's entries in the order they're written: the binary tree over
 * the sorted names, each subtree's root before it (the root at offset 0). */
static void tree_order(const Node *dir, unsigned lo, unsigned hi, unsigned *order, unsigned *n)
{
    if (lo >= hi) {
        return;
    }
    const unsigned mid = lo + (hi - lo) / 2;
    order[(*n)++] = mid;
    tree_order(dir, lo, mid, order, n);
    tree_order(dir, mid + 1, hi, order, n);
}

/* A folder's table: its bytes in *out (size in dir->size, whole sectors) */
static bool table_build(Node *dir, uint8_t **out)
{
    if (!dir->nkids) {
        dir->size = SECTOR;
        *out = malloc(SECTOR);
        if (*out) {
            memset(*out, 0xFF, SECTOR);
        }
        return *out != NULL;
    }
    unsigned *order = malloc(dir->nkids * sizeof(unsigned)), n = 0;
    uint32_t *off = malloc(dir->nkids * sizeof(uint32_t));
    if (!order || !off) {
        free(order);
        free(off);
        return false;
    }
    tree_order(dir, 0, dir->nkids, order, &n);
    /* offsets: an entry that would cross a sector's end starts the next */
    uint32_t at = 0;
    for (unsigned i = 0; i < n; i++) {
        const uint32_t len = entry_len(&dir->kids[order[i]]);
        if (at / SECTOR != (at + len - 1) / SECTOR) {
            at = (at / SECTOR + 1) * SECTOR;
        }
        off[order[i]] = at;
        at += len;
    }
    const uint32_t size = (at + SECTOR - 1) / SECTOR * SECTOR;
    /* an entry's links are 16 bits, in units of 4 bytes: a table past
     * 256 KiB (thousands of files in one folder) can't name its entries,
     * and was written with links cut short (files not found, or others) */
    uint8_t *t = at <= 0x40000u ? malloc(size) : NULL;
    if (!t) {
        if (at > 0x40000u) {
            nc_log("hakuX-native", "disc: the folder %s has more files (%u) than a disc's folder can list",
                   dir->name[0] ? dir->name : "of the game", dir->nkids);
        }
        free(order);
        free(off);
        return false;
    }
    memset(t, 0xFF, size);
    /* each node's children in the tree: the middles of its halves */
    struct Range {
        unsigned lo, hi;
    } *st = malloc(dir->nkids * sizeof(struct Range));
    unsigned sp = 0;
    if (st) {
        st[sp++] = (struct Range){0, dir->nkids};
    }
    while (sp) {
        const struct Range r = st[--sp];
        const unsigned mid = r.lo + (r.hi - r.lo) / 2;
        const Node *k = &dir->kids[mid];
        uint8_t *e = t + off[mid];
        const unsigned lmid = r.lo + (mid - r.lo) / 2, rmid = mid + 1 + (r.hi - mid - 1) / 2;
        const uint16_t left = mid > r.lo ? (uint16_t)(off[lmid] / 4) : 0;
        const uint16_t right = mid + 1 < r.hi ? (uint16_t)(off[rmid] / 4) : 0;
        const uint32_t sec = k->sector, sz = (uint32_t)k->size;
        const size_t nl = strlen(k->name);
        memset(e, 0, entry_len(k));
        memcpy(e, &left, 2);
        memcpy(e + 2, &right, 2);
        memcpy(e + 4, &sec, 4);
        memcpy(e + 8, &sz, 4);
        e[12] = k->dir ? 0x10 : 0x20;       /* directory, or archive */
        e[13] = (uint8_t)nl;
        memcpy(e + 14, k->name, nl);
        /* the padding after the name: 0xFF, as writers leave it */
        memset(e + 14 + nl, 0xFF, entry_len(k) - 14 - nl);
        if (mid > r.lo) {
            st[sp++] = (struct Range){r.lo, mid};
        }
        if (mid + 1 < r.hi) {
            st[sp++] = (struct Range){mid + 1, r.hi};
        }
    }
    free(st);
    free(order);
    free(off);
    dir->size = size;
    *out = t;
    return true;
}

/* the tables' sizes, then sectors: each folder's table (sectors from *next) */
static bool tables_size(Node *dir, uint32_t *next)
{
    uint8_t *t;
    if (!table_build(dir, &t)) {      /* the size only: built again once sectors are known */
        return false;
    }
    free(t);
    dir->sector = *next;
    *next += (uint32_t)(dir->size / SECTOR);
    for (unsigned i = 0; i < dir->nkids; i++) {
        if (dir->kids[i].dir && !tables_size(&dir->kids[i], next)) {
            return false;
        }
    }
    return true;
}

static void files_place(Node *dir, uint32_t *next, FFile *files, unsigned *n)
{
    for (unsigned i = 0; i < dir->nkids; i++) {
        Node *k = &dir->kids[i];
        if (!k->dir) {
            k->sector = k->size ? *next : 0;
            *next += (uint32_t)((k->size + SECTOR - 1) / SECTOR);
            if (k->size) {
                files[(*n)++] = (FFile){k->host, k->sector, (uint32_t)k->size};
                k->host = NULL;                 /* the file list owns it now */
            }
        } else {
            files_place(k, next, files, n);
        }
    }
}

/* the tables, written where their sectors are (their entries' sectors known) */
static bool tables_write_all(Node *dir, uint8_t *meta)
{
    uint8_t *t;
    if (!table_build(dir, &t)) {
        return false;
    }
    memcpy(meta + (uint64_t)dir->sector * SECTOR, t, dir->size);
    free(t);
    for (unsigned i = 0; i < dir->nkids; i++) {
        if (dir->kids[i].dir && !tables_write_all(&dir->kids[i], meta)) {
            return false;
        }
    }
    return true;
}

/* The folder of an open file: its path, from the descriptor */
static bool fd_dir(int fd, char *out, size_t cap)
{
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    char p[MAX_PATH * 4];
    const DWORD n = h != INVALID_HANDLE_VALUE ? GetFinalPathNameByHandleA(h, p, sizeof(p), 0) : 0;
    if (!n || n >= sizeof(p)) {
        return false;
    }
    const char *s = p;
    char unc[MAX_PATH * 4];
    if (!strncmp(s, "\\\\?\\UNC\\", 8)) {
        snprintf(unc, sizeof(unc), "\\\\%s", s + 8);    /* \\server\share\... */
        s = unc;
    } else if (!strncmp(s, "\\\\?\\", 4)) {
        s += 4;
    }
    snprintf(out, cap, "%s", s);
#else
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const ssize_t n = readlink(link, out, cap - 1);
    if (n <= 0) {
        return false;
    }
    out[n] = 0;
#endif
    char *slash = strrchr(out, '/'), *bs = strrchr(out, '\\');
    if (bs > slash) {
        slash = bs;
    }
    if (!slash) {
        return false;
    }
    *slash = 0;
    return true;
}

bool nc_fold_attach(int fd)
{
    char magic[4];
    if (fd < 0 || pread(fd, magic, 4, 0) != 4 || memcmp(magic, "XBEH", 4)) {
        return false;
    }
    pthread_mutex_lock(&g_mu);
    if (find(fd)) {
        pthread_mutex_unlock(&g_mu);
        return true;
    }
    Fold *f = NULL;
    for (int i = 0; i < MAX_FOLDS && !f; i++) {
        f = g_fold[i].fd < 0 ? &g_fold[i] : NULL;
    }
    char dir[1024];
    if (!f || !fd_dir(fd, dir, sizeof(dir))) {
        pthread_mutex_unlock(&g_mu);
        nc_log("hakuX-native", "disc: a game folder can't be read (%s)", f ? "its path" : "too many open");
        return false;
    }
    Node root;
    memset(&root, 0, sizeof(root));
    unsigned count = 0;
    uint32_t next = 33;
    bool ok = scan(&root, dir, 0, &count) && tables_size(&root, &next);
    const uint32_t meta_sectors = next;
    FFile *files = ok ? calloc(count ? count : 1, sizeof(FFile)) : NULL;
    unsigned nfiles = 0;
    uint8_t *meta = NULL;
    if (files) {
        files_place(&root, &next, files, &nfiles);
        meta = calloc(meta_sectors, SECTOR);
    }
    ok = ok && meta && tables_write_all(&root, meta);
    if (ok) {
        uint8_t *vd = meta + 32ull * SECTOR;
        static const char k_magic[] = "MICROSOFT*XBOX*MEDIA";
        memcpy(vd, k_magic, 20);
        memcpy(vd + 20, &root.sector, 4);
        const uint32_t rs = (uint32_t)root.size;
        memcpy(vd + 24, &rs, 4);
        memcpy(vd + 0x7EC, k_magic, 20);
    }
    node_free(&root);
    if (!ok) {
        for (unsigned i = 0; i < nfiles; i++) {
            free(files[i].host);
        }
        free(files);
        free(meta);
        pthread_mutex_unlock(&g_mu);
        nc_log("hakuX-native", "disc: the game folder %s can't be read", dir);
        return false;
    }
    memset(f, 0, sizeof(*f));
    f->fd = fd;
    f->meta = meta;
    f->meta_sectors = meta_sectors;
    f->files = files;
    f->nfiles = nfiles;
    f->size = (uint64_t)next * SECTOR;
    pthread_mutex_init(&f->mu, NULL);
    for (int i = 0; i < FD_CACHE; i++) {
        f->cache_fd[i] = -1;
    }
    pthread_mutex_unlock(&g_mu);
    nc_log("hakuX-native", "disc: the game folder %s (%u files, %llu MiB) as a disc", dir, nfiles,
           (unsigned long long)(f->size >> 20));
    return true;
}

void nc_fold_detach(int fd)
{
    pthread_mutex_lock(&g_mu);
    Fold *f = find(fd);
    if (f) {
        for (int i = 0; i < FD_CACHE; i++) {
            if (f->cache_fd[i] >= 0) {
                close(f->cache_fd[i]);
            }
        }
        for (unsigned i = 0; i < f->nfiles; i++) {
            free(f->files[i].host);
        }
        free(f->files);
        free(f->meta);
        pthread_mutex_destroy(&f->mu);
        f->fd = -1;
    }
    pthread_mutex_unlock(&g_mu);
}

bool nc_fold_is(int fd)
{
    pthread_mutex_lock(&g_mu);
    const bool r = find(fd) != NULL;
    pthread_mutex_unlock(&g_mu);
    return r;
}

/* f->mu held: file i's descriptor, opened if it isn't */
static int file_fd(Fold *f, unsigned i)
{
    for (int c = 0; c < FD_CACHE; c++) {
        if (f->cache_fd[c] >= 0 && f->cache_file[c] == i) {
            return f->cache_fd[c];
        }
    }
    const unsigned slot = f->next++ % FD_CACHE;
    if (f->cache_fd[slot] >= 0) {
        close(f->cache_fd[slot]);
    }
#ifdef O_BINARY
    f->cache_fd[slot] = open(f->files[i].host, O_RDONLY | O_BINARY);
#else
    f->cache_fd[slot] = open(f->files[i].host, O_RDONLY);
#endif
    f->cache_file[slot] = i;
    return f->cache_fd[slot];
}

bool nc_fold_read(int fd, void *buf, size_t len, uint64_t off)
{
    pthread_mutex_lock(&g_mu);
    Fold *f = find(fd);
    pthread_mutex_unlock(&g_mu);
    if (!f) {
        return false;
    }
    uint8_t *p = buf;
    bool ok = true;
    pthread_mutex_lock(&f->mu);
    while (len && ok) {
        const uint64_t meta_end = (uint64_t)f->meta_sectors * SECTOR;
        if (off < meta_end) {
            const size_t n = (size_t)(len < meta_end - off ? len : meta_end - off);
            memcpy(p, f->meta + off, n);
            p += n, off += n, len -= n;
            continue;
        }
        /* the last file starting at or before off's sector */
        const uint32_t sec = (uint32_t)(off / SECTOR);
        unsigned lo = 0, hi = f->nfiles;
        while (lo < hi) {
            const unsigned mid = (lo + hi) / 2;
            if (f->files[mid].sector <= sec) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        const FFile *ff = lo ? &f->files[lo - 1] : NULL;
        const uint64_t start = ff ? (uint64_t)ff->sector * SECTOR : 0;
        const uint64_t end = ff ? start + (uint64_t)(ff->size + SECTOR - 1) / SECTOR * SECTOR : 0;
        if (ff && off < end) {
            /* the file's bytes, then the zeros to its last sector's end */
            const uint64_t fend = start + ff->size;
            size_t n = (size_t)(len < end - off ? len : end - off);
            if (off < fend) {
                const size_t m = (size_t)(n < fend - off ? n : fend - off);
                const int hfd = file_fd(f, lo - 1);
                size_t got = 0;
                while (hfd >= 0 && got < m) {
                    const ssize_t r = pread(hfd, p + got, m - got, (off_t)(off - start + got));
                    if (r <= 0) {
                        break;
                    }
                    got += (size_t)r;
                }
                if (got < m) {
                    nc_log("hakuX-native", "disc: reading %s failed", ff->host);
                    ok = false;
                    break;
                }
                n = m;
            } else {
                memset(p, 0, n);
            }
            p += n, off += n, len -= n;
            continue;
        }
        /* between files or past the last: zeros up to the next file */
        const uint64_t next = lo < f->nfiles ? (uint64_t)f->files[lo].sector * SECTOR : UINT64_MAX;
        const size_t n = (size_t)(len < next - off ? len : next - off);
        memset(p, 0, n);
        p += n, off += n, len -= n;
    }
    pthread_mutex_unlock(&f->mu);
    return ok;
}

bool nc_fold_xbe_path(const char *path, char *out, size_t cap)
{
    struct stat st;
    if (!path || stat(path, &st)) {
        return false;
    }
    char p[1024];
    if (S_ISDIR(st.st_mode)) {
        const size_t n = strlen(path);
        const bool sep = n && (path[n - 1] == '/' || path[n - 1] == '\\');
        snprintf(p, sizeof(p), "%s%sdefault.xbe", path, sep ? "" : "/");
        if (stat(p, &st) || S_ISDIR(st.st_mode)) {
            return false;
        }
    } else {
        const char *b = strrchr(path, '/'), *b2 = strrchr(path, '\\');
        b = b2 > b ? b2 : b;
        if (strcasecmp(b ? b + 1 : path, "default.xbe")) {
            return false;
        }
        snprintf(p, sizeof(p), "%s", path);
    }
    snprintf(out, cap, "%s", p);
    return true;
}
