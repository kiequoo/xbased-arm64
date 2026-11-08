/*
 * hakuX native core — CHD disc images (chd_image.h).
 *
 * libchdr reads the file through callbacks over the descriptor (pread, so
 * no shared file position); each image's hunk reads go one at a time under
 * its lock, the last two hunks kept (a sector read straddling two, a file
 * read in small pieces).
 */
#include "native/core/chd_image.h"
#include "native/core/nc.h"
#include "native/core/xdvdfs.h"

#ifndef NC_HAVE_CHD
/* built without libchdr: no CHD image is one */
bool nc_chd_attach(int fd)
{
    return false;
}
void nc_chd_detach(int fd)
{
}
bool nc_chd_is(int fd)
{
    return false;
}
bool nc_chd_read(int fd, void *buf, size_t len, uint64_t off)
{
    return false;
}
#else
#include <libchdr/cdrom.h>
#include <libchdr/chd.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_CHDS 4

typedef struct Chd {
    int fd;                 /* -1: free */
    uint64_t pos;           /* the callbacks' file position */
    chd_file *chd;
    uint32_t hunk_bytes;
    uint64_t size;          /* the image's bytes */
    uint32_t unit_bytes;    /* a CD's frames (createcd): 2448, each sector's 2048 */
    uint32_t data_off;      /* bytes at data_off in it; 0: a DVD's (createdvd) */
    pthread_mutex_t mu;
    uint8_t *cache[2];
    int64_t cached[2];      /* the hunks there (-1: none) */
    unsigned next;
} Chd;

static Chd g_chd[MAX_CHDS] = {{-1}, {-1}, {-1}, {-1}};
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t cb_size(void *p)
{
    struct stat st;
    return fstat(((Chd *)p)->fd, &st) == 0 ? (uint64_t)st.st_size : (uint64_t)-1;
}

static size_t cb_read(void *buf, size_t size, size_t count, void *p)
{
    Chd *c = p;
    size_t want = size * count, got = 0;
    while (got < want) {
        const ssize_t n = pread(c->fd, (uint8_t *)buf + got, want - got, (off_t)(c->pos + got));
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    c->pos += got;
    return size ? got / size : 0;
}

static int cb_close(void *p)
{
    (void)p;
    return 0;               /* the descriptor is its owner's */
}

static int cb_seek(void *p, int64_t off, int whence)
{
    Chd *c = p;
    if (whence == SEEK_SET) {
        c->pos = (uint64_t)off;
    } else if (whence == SEEK_CUR) {
        c->pos += (uint64_t)off;
    } else {
        c->pos = cb_size(p) + (uint64_t)off;
    }
    return 0;
}

static const core_file_callbacks k_cb = {cb_size, cb_read, cb_close, cb_seek};

static Chd *find(int fd)
{
    for (int i = 0; i < MAX_CHDS; i++) {
        if (g_chd[i].fd == fd && fd >= 0) {
            return &g_chd[i];
        }
    }
    return NULL;
}

/* A CD's CHD (chdman createcd, what most guides give: an ISO taken as one
 * mode 1 track): each 2048-byte sector in a 2448-byte frame (2352 and the
 * subcode's 96), at its start for a cooked track, after the sync and header
 * for a raw one. The image is the first track's sectors. False: a DVD's or a
 * hard disk's, the bytes as they are. */
static bool cd_track(Chd *c, const chd_header *h)
{
    static const uint32_t tags[] = {CDROM_TRACK_METADATA2_TAG, CDROM_TRACK_METADATA_TAG, GDROM_TRACK_METADATA_TAG};
    char meta[256];
    uint32_t len = 0;
    bool found = false;
    for (unsigned i = 0; i < sizeof(tags) / sizeof(tags[0]) && !found; i++) {
        found = chd_get_metadata(c->chd, tags[i], 0, meta, sizeof(meta) - 1, &len, NULL, NULL) == CHDERR_NONE;
    }
    if (!found || h->unitbytes != CD_FRAME_SIZE || h->hunkbytes % CD_FRAME_SIZE) {
        return false;
    }
    meta[len < sizeof(meta) ? len : sizeof(meta) - 1] = 0;
    char type[32] = "";
    int frames = 0;
    const char *t = strstr(meta, "TYPE:"), *f = strstr(meta, "FRAMES:");
    if (t) {
        sscanf(t + 5, "%31s", type);
    }
    if (f) {
        frames = atoi(f + 7);
    }
    uint32_t off;
    if (!strcmp(type, "MODE1") || !strcmp(type, "MODE2_FORM1")) {
        off = 0;                         /* cooked: the 2048 bytes alone */
    } else if (!strcmp(type, "MODE1_RAW")) {
        off = 16;                        /* sync 12, header 4 */
    } else if (!strcmp(type, "MODE2_RAW")) {
        off = 24;                        /* and form 1's subheader 8 */
    } else {
        nc_log("hakuX-native", "disc: CHD image of a CD whose first track is %s, not data", type[0] ? type : "unknown");
        return false;
    }
    const uint64_t units = h->logicalbytes / CD_FRAME_SIZE;
    c->unit_bytes = CD_FRAME_SIZE;
    c->data_off = off;
    c->size = (uint64_t)(frames > 0 && (uint64_t)frames < units ? (uint64_t)frames : units) * NC_XDVDFS_SECTOR;
    return true;
}

bool nc_chd_attach(int fd)
{
    char magic[8];
    if (fd < 0 || pread(fd, magic, 8, 0) != 8 || memcmp(magic, "MComprHD", 8)) {
        return false;
    }
    pthread_mutex_lock(&g_mu);
    Chd *c = find(fd);
    if (c) {
        pthread_mutex_unlock(&g_mu);
        return true;
    }
    for (int i = 0; i < MAX_CHDS && !c; i++) {
        c = g_chd[i].fd < 0 ? &g_chd[i] : NULL;
    }
    if (!c) {
        pthread_mutex_unlock(&g_mu);
        nc_log("hakuX-native", "disc: too many CHD images open");
        return false;
    }
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    const chd_error e = chd_open_core_file_callbacks(&k_cb, c, CHD_OPEN_READ, NULL, &c->chd);
    const chd_header *h = e == CHDERR_NONE ? chd_get_header(c->chd) : NULL;
    if (!h || !h->hunkbytes) {
        nc_log("hakuX-native", "disc: CHD image unreadable (%s)", chd_error_string(e));
        if (e == CHDERR_NONE) {
            chd_close(c->chd);
        }
        c->fd = -1;
        pthread_mutex_unlock(&g_mu);
        return false;
    }
    c->hunk_bytes = h->hunkbytes;
    c->size = h->logicalbytes;
    const bool cd = cd_track(c, h);
    pthread_mutex_init(&c->mu, NULL);
    for (int i = 0; i < 2; i++) {
        c->cache[i] = malloc(c->hunk_bytes);
        c->cached[i] = -1;
    }
    pthread_mutex_unlock(&g_mu);
    nc_log("hakuX-native", "disc: CHD image%s, %llu MiB in %u-byte hunks", cd ? " (a CD's, chdman createcd)" : "",
           (unsigned long long)(c->size >> 20), c->hunk_bytes);
    return true;
}

void nc_chd_detach(int fd)
{
    pthread_mutex_lock(&g_mu);
    Chd *c = find(fd);
    if (c) {
        chd_close(c->chd);
        free(c->cache[0]);
        free(c->cache[1]);
        pthread_mutex_destroy(&c->mu);
        c->fd = -1;
    }
    pthread_mutex_unlock(&g_mu);
}

bool nc_chd_is(int fd)
{
    pthread_mutex_lock(&g_mu);
    const bool r = find(fd) != NULL;
    pthread_mutex_unlock(&g_mu);
    return r;
}

/* c->mu held: hunk n's bytes */
static const uint8_t *hunk(Chd *c, uint64_t n)
{
    for (int i = 0; i < 2; i++) {
        if (c->cached[i] == (int64_t)n) {
            return c->cache[i];
        }
    }
    const unsigned slot = c->next++ & 1;
    if (!c->cache[slot] || chd_read(c->chd, (uint32_t)n, c->cache[slot]) != CHDERR_NONE) {
        c->cached[slot] = -1;
        return NULL;
    }
    c->cached[slot] = (int64_t)n;
    return c->cache[slot];
}

bool nc_chd_read(int fd, void *buf, size_t len, uint64_t off)
{
    pthread_mutex_lock(&g_mu);
    Chd *c = find(fd);
    pthread_mutex_unlock(&g_mu);
    if (!c) {
        return false;
    }
    uint8_t *p = buf;
    bool ok = true;
    pthread_mutex_lock(&c->mu);
    while (len && ok) {
        if (off >= c->size) {
            memset(p, 0, len);              /* past the image: zeros, as for a trimmed one */
            break;
        }
        /* where off is in the CHD's bytes, and how much from there in a row:
         * a DVD's to the hunk's end, a CD's to its sector's (its frame
         * never straddles two hunks) */
        uint64_t at = off, run = c->hunk_bytes - off % c->hunk_bytes;
        if (c->unit_bytes) {
            const uint64_t sector = off / NC_XDVDFS_SECTOR, in_sector = off % NC_XDVDFS_SECTOR;
            at = sector * c->unit_bytes + c->data_off + in_sector;
            run = NC_XDVDFS_SECTOR - in_sector;
        }
        const uint64_t n = at / c->hunk_bytes;
        const uint32_t in = (uint32_t)(at % c->hunk_bytes);
        uint64_t take = run;
        take = take > len ? len : take;
        take = off + take > c->size ? c->size - off : take;
        const uint8_t *h = hunk(c, n);
        if (!h) {
            nc_log("hakuX-native", "disc: CHD hunk %llu unreadable", (unsigned long long)n);
            ok = false;
            break;
        }
        memcpy(p, h + in, (size_t)take);
        p += take;
        len -= (size_t)take;
        off += take;
    }
    pthread_mutex_unlock(&c->mu);
    return ok;
}
#endif /* NC_HAVE_CHD */
