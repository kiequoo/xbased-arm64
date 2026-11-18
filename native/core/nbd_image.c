/*
 * xbased native core — a disc image over NBD (nbd_image.h): the fixed
 * newstyle handshake (NBD_OPT_GO, else NBD_OPT_EXPORT_NAME), then simple
 * READ requests, one at a time on one connection (a lock around each),
 * made again once if it drops.
 */
#include "native/core/nbd_image.h"
#include "native/core/nc.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include "nc_win_net.h"
typedef SOCKET NbdSock;
#define NBD_NO_SOCK INVALID_SOCKET
#define nbd_sock_close(s) nc_sock_close(s)
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
typedef int NbdSock;
#define NBD_NO_SOCK (-1)
#define nbd_sock_close(s) close(s)
#endif

static const char k_marker[] = "XBASED-NBD 1\n";

#define NBD_OPT_EXPORT_NAME 1u
#define NBD_OPT_GO          7u
#define NBD_REP_ACK         1u
#define NBD_REP_INFO        3u
#define NBD_INFO_EXPORT     0u
#define NBD_FLAG_FIXED_NEWSTYLE 1u
#define NBD_FLAG_NO_ZEROES      2u
#define NBD_REQUEST_MAGIC   0x25609513u
#define NBD_REPLY_MAGIC     0x67446698u
#define NBD_CHUNK           (1u << 20)

typedef struct Conn {
    char uri[512];
    NbdSock s;
    uint64_t size;
    uint64_t handle;
    int refs;
    pthread_mutex_t mu;
} Conn;

#define MAX_FDS 32
static struct {
    int fd;
    Conn *c;
} g_fds[MAX_FDS];
static Conn *g_conn;           /* the one image served (a disc at a time) */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

bool nc_nbd_is_uri(const char *path)
{
    return path && !strncmp(path, "nbd://", 6);
}

bool nc_nbd_marker_write(const char *path, const char *uri)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    const bool ok = fputs(k_marker, f) >= 0 && fputs(uri, f) >= 0 && fputs("\n", f) >= 0;
    return fclose(f) == 0 && ok;
}

static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = v << 8 | p[i];
    }
    return v;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool send_all(NbdSock s, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        const int n = (int)send(s, p, (int)(len > (1u << 30) ? (1u << 30) : len), 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool recv_all(NbdSock s, void *buf, size_t len)
{
    char *p = buf;
    while (len) {
        const int n = (int)recv(s, p, (int)(len > (1u << 30) ? (1u << 30) : len), 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

/* nbd://host[:port][/export]: host, port, export name */
static bool parse_uri(const char *uri, char *host, size_t hn, char *port, size_t pn, char *name, size_t nn)
{
    if (!nc_nbd_is_uri(uri)) {
        return false;
    }
    const char *h = uri + 6;
    const char *slash = strchr(h, '/');
    const size_t hl = slash ? (size_t)(slash - h) : strlen(h);
    char hp[300];
    if (!hl || hl >= sizeof(hp)) {
        return false;
    }
    memcpy(hp, h, hl);
    hp[hl] = 0;
    snprintf(name, nn, "%s", slash ? slash + 1 : "");
    snprintf(port, pn, "10809");
    char *colon = NULL;
    if (hp[0] == '[') {
        char *end = strchr(hp, ']');    /* [IPv6]:port */
        if (!end) {
            return false;
        }
        *end = 0;
        snprintf(host, hn, "%s", hp + 1);
        colon = end[1] == ':' ? end + 1 : NULL;
    } else {
        colon = strrchr(hp, ':');
        if (colon) {
            *colon = 0;
        }
        snprintf(host, hn, "%s", hp);
    }
    if (colon && colon[1]) {
        snprintf(port, pn, "%s", colon + 1);
    }
    return host[0] != 0;
}

/* The connection to c->uri, through the handshake: c->s and c->size set. */
static bool connect_conn(Conn *c)
{
    char host[256], port[16], name[256];
    if (!parse_uri(c->uri, host, sizeof(host), port, sizeof(port), name, sizeof(name))) {
        nc_log("xbased-native", "disc: %s is not an nbd://host[:port][/export] address", c->uri);
        return false;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        nc_log("xbased-native", "disc: NBD server %s:%s not found", host, port);
        return false;
    }
    NbdSock s = NBD_NO_SOCK;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == NBD_NO_SOCK) {
            continue;
        }
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) {
            break;
        }
        nbd_sock_close(s);
        s = NBD_NO_SOCK;
    }
    freeaddrinfo(res);
    if (s == NBD_NO_SOCK) {
        nc_log("xbased-native", "disc: can't connect to the NBD server at %s:%s", host, port);
        return false;
    }
    const int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    uint8_t hello[18];
    if (!recv_all(s, hello, sizeof(hello)) || memcmp(hello, "NBDMAGIC", 8) || memcmp(hello + 8, "IHAVEOPT", 8)) {
        nc_log("xbased-native", "disc: %s doesn't speak the NBD newstyle handshake", c->uri);
        nbd_sock_close(s);
        return false;
    }
    const uint16_t sflags = (uint16_t)(hello[16] << 8 | hello[17]);
    const bool no_zeroes = sflags & NBD_FLAG_NO_ZEROES;
    uint8_t cf[4];
    put32(cf, (sflags & NBD_FLAG_FIXED_NEWSTYLE) | (no_zeroes ? NBD_FLAG_NO_ZEROES : 0));
    bool ok = send_all(s, cf, 4);
    const uint32_t nl = (uint32_t)strlen(name);
    bool have = false;
    if (ok && (sflags & NBD_FLAG_FIXED_NEWSTYLE)) {
        /* NBD_OPT_GO: the export's name, no information asked beyond the default */
        uint8_t opt[16 + 4 + 256 + 2];
        memcpy(opt, "IHAVEOPT", 8);
        put32(opt + 8, NBD_OPT_GO);
        put32(opt + 12, 4 + nl + 2);
        put32(opt + 16, nl);
        memcpy(opt + 20, name, nl);
        opt[20 + nl] = opt[21 + nl] = 0;
        ok = send_all(s, opt, 22 + nl);
        while (ok) {
            uint8_t rep[20];
            if (!(ok = recv_all(s, rep, sizeof(rep)))) {
                break;
            }
            const uint32_t type = be32(rep + 12), len = be32(rep + 16);
            uint8_t data[512];
            if (len > sizeof(data)) {
                ok = false;
                break;
            }
            if (len && !(ok = recv_all(s, data, len))) {
                break;
            }
            if (type == NBD_REP_INFO && len >= 12 && (data[0] << 8 | data[1]) == NBD_INFO_EXPORT) {
                c->size = be64(data + 2);
            } else if (type == NBD_REP_ACK) {
                have = true;
                break;
            } else if (type & 0x80000000u) {
                break;      /* an error: the old option below */
            }
        }
    }
    if (ok && !have) {
        /* NBD_OPT_EXPORT_NAME: the transmission starts at once */
        uint8_t opt[16 + 256];
        memcpy(opt, "IHAVEOPT", 8);
        put32(opt + 8, NBD_OPT_EXPORT_NAME);
        put32(opt + 12, nl);
        memcpy(opt + 16, name, nl);
        uint8_t tail[10 + 124];
        ok = send_all(s, opt, 16 + nl) && recv_all(s, tail, no_zeroes ? 10 : sizeof(tail));
        if (ok) {
            c->size = be64(tail);
            have = true;
        }
    }
    if (!ok || !have || !c->size) {
        nc_log("xbased-native", "disc: the NBD server at %s didn't offer its export", c->uri);
        nbd_sock_close(s);
        return false;
    }
    c->s = s;
    nc_log("xbased-native", "disc: NBD export %s, %llu MiB", c->uri, (unsigned long long)(c->size >> 20));
    return true;
}

static Conn *conn_of(int fd)
{
    for (int i = 0; i < MAX_FDS; i++) {
        if (g_fds[i].c && g_fds[i].fd == fd) {
            return g_fds[i].c;
        }
    }
    return NULL;
}

bool nc_nbd_attach(int fd)
{
    char head[sizeof(k_marker) - 1 + 512 + 2];
    memset(head, 0, sizeof(head));
    const ssize_t n = fd >= 0 ? pread(fd, head, sizeof(head) - 1, 0) : -1;
    if (n < (ssize_t)sizeof(k_marker) || memcmp(head, k_marker, sizeof(k_marker) - 1)) {
        return false;
    }
    char *uri = head + sizeof(k_marker) - 1;
    uri[strcspn(uri, "\r\n")] = 0;
    pthread_mutex_lock(&g_mu);
    Conn *c = g_conn;
    if (c && strcmp(c->uri, uri)) {
        c = NULL;       /* another image's (one at a time) */
    }
    if (!c && !g_conn) {
        c = calloc(1, sizeof(*c));
        if (c) {
            snprintf(c->uri, sizeof(c->uri), "%s", uri);
            c->s = NBD_NO_SOCK;
            pthread_mutex_init(&c->mu, NULL);
            if (!connect_conn(c)) {
                pthread_mutex_destroy(&c->mu);
                free(c);
                c = NULL;
            } else {
                g_conn = c;
            }
        }
    }
    int slot = -1;
    for (int i = 0; c && i < MAX_FDS && slot < 0; i++) {
        if (!g_fds[i].c) {
            slot = i;
        }
    }
    if (slot >= 0) {
        g_fds[slot].fd = fd;
        g_fds[slot].c = c;
        c->refs++;
    }
    pthread_mutex_unlock(&g_mu);
    return slot >= 0;
}

void nc_nbd_detach(int fd)
{
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_FDS; i++) {
        if (g_fds[i].c && g_fds[i].fd == fd) {
            Conn *c = g_fds[i].c;
            g_fds[i].c = NULL;
            if (--c->refs == 0) {
                if (c->s != NBD_NO_SOCK) {
                    nbd_sock_close(c->s);
                }
                pthread_mutex_destroy(&c->mu);
                if (g_conn == c) {
                    g_conn = NULL;
                }
                free(c);
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

bool nc_nbd_is(int fd)
{
    pthread_mutex_lock(&g_mu);
    const bool is = conn_of(fd) != NULL;
    pthread_mutex_unlock(&g_mu);
    return is;
}

uint64_t nc_nbd_size(int fd)
{
    pthread_mutex_lock(&g_mu);
    Conn *c = conn_of(fd);
    const uint64_t size = c ? c->size : 0;
    pthread_mutex_unlock(&g_mu);
    return size;
}

/* One READ of len bytes (len <= NBD_CHUNK) at off. */
static bool read_once(Conn *c, uint8_t *buf, uint32_t len, uint64_t off)
{
    uint8_t req[28];
    put32(req, NBD_REQUEST_MAGIC);
    req[4] = req[5] = 0;    /* flags */
    req[6] = req[7] = 0;    /* NBD_CMD_READ */
    put64(req + 8, ++c->handle);
    put64(req + 16, off);
    put32(req + 24, len);
    uint8_t rep[16];
    if (!send_all(c->s, req, sizeof(req)) || !recv_all(c->s, rep, sizeof(rep)) || be32(rep) != NBD_REPLY_MAGIC ||
        be64(rep + 8) != c->handle) {
        return false;
    }
    if (be32(rep + 4)) {
        nc_log("xbased-native", "disc: the NBD server failed a read at +0x%llx (error %u)", (unsigned long long)off,
               be32(rep + 4));
        return false;
    }
    return recv_all(c->s, buf, len);
}

bool nc_nbd_read(int fd, void *buf, size_t len, uint64_t off)
{
    pthread_mutex_lock(&g_mu);
    Conn *c = conn_of(fd);
    pthread_mutex_unlock(&g_mu);
    if (!c) {
        return false;
    }
    uint8_t *p = buf;
    pthread_mutex_lock(&c->mu);
    bool ok = true;
    while (len && ok) {
        if (off >= c->size) {
            memset(p, 0, len);  /* zeros past the image's end, as a file's reads give */
            break;
        }
        uint64_t n = len < NBD_CHUNK ? len : NBD_CHUNK;
        if (n > c->size - off) {
            n = c->size - off;
        }
        ok = c->s != NBD_NO_SOCK && read_once(c, p, (uint32_t)n, off);
        if (!ok) {
            /* the connection lost: made again once (the server restarted, a drive's read retried) */
            if (c->s != NBD_NO_SOCK) {
                nbd_sock_close(c->s);
                c->s = NBD_NO_SOCK;
            }
            ok = connect_conn(c) && read_once(c, p, (uint32_t)n, off);
        }
        p += n;
        len -= (size_t)n;
        off += n;
    }
    pthread_mutex_unlock(&c->mu);
    return ok;
}
