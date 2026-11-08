/*
 * hakuX on Windows — the POSIX functions nc_win_posix.h and the compat
 * headers declare, on Win32.
 */
#include <windows.h>
#include <mmsystem.h>
#include <bcrypt.h>
#include <stdbool.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>

#include <errno.h>
#include <io.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/system_properties.h>
#include "nc_win_net.h"

/* ---------------------------------------------------------------- clock */

static bool g_tsc_ok;
static uint64_t g_tsc_base, g_ns_base, g_tsc_mult; /* ns = base + (tsc - base) * mult >> 32 */

static uint64_t rdtsc_now(void)
{
    unsigned lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static uint64_t qpc_ns(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) {
        QueryPerformanceFrequency(&f);
    }
    QueryPerformanceCounter(&c);
    return (uint64_t)((unsigned __int128)c.QuadPart * 1000000000u / (uint64_t)f.QuadPart);
}

/* The TSC is used when it is invariant (CPUID 8000_0007h EDX bit 8: the
 * same rate in every power state, on every core), its rate measured against
 * QPC over 50 ms. */
static void clock_init(void)
{
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
    if (a < 0x80000007u) {
        return;
    }
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000007u), "c"(0));
    if (!(d & (1u << 8))) {
        return;
    }
    const uint64_t n0 = qpc_ns(), t0 = rdtsc_now();
    uint64_t n1;
    do {
        n1 = qpc_ns();
    } while (n1 - n0 < 50000000ull);
    const uint64_t t1 = rdtsc_now();
    if (t1 <= t0) {
        return;
    }
    g_tsc_mult = (uint64_t)(((unsigned __int128)(n1 - n0) << 32) / (t1 - t0));
    g_tsc_base = t1;
    g_ns_base = n1;
    g_tsc_ok = true;
}

int nc_clock_gettime(clockid_t clk, struct timespec *ts)
{
    uint64_t ns;
    if (clk == CLOCK_REALTIME) {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        const uint64_t t = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull;
        ts->tv_sec = (time_t)(t / 10000000ull);
        ts->tv_nsec = (long)(t % 10000000ull) * 100;
        return 0;
    }
    if (clk == CLOCK_MONOTONIC_COARSE) {
        /* KUSER_SHARED_DATA's InterruptTime (100 ns, since boot), stepped at
         * the clock interrupt (1 ms with timeBeginPeriod(1)), as Linux's
         * coarse clock steps at the tick: what the kernel caches its time
         * by (ke.c nc_ke_now_ns) changes no oftener than that. */
        volatile const uint32_t *it = (volatile const uint32_t *)(uintptr_t)0x7FFE0008u;
        uint32_t hi1, lo, hi2;
        do {
            hi1 = it[1];
            lo = it[0];
            hi2 = it[2];
        } while (hi1 != hi2);
        ns = ((uint64_t)hi1 << 32 | lo) * 100;
        ts->tv_sec = (time_t)(ns / 1000000000ull);
        ts->tv_nsec = (long)(ns % 1000000000ull);
        return 0;
    }
    if (clk != CLOCK_MONOTONIC) {
        return clock_gettime64(clk, (struct _timespec64 *)ts);
    }
    if (g_tsc_ok) {
        ns = g_ns_base + (uint64_t)(((unsigned __int128)(rdtsc_now() - g_tsc_base) * g_tsc_mult) >> 32);
    } else {
        ns = qpc_ns();
    }
    ts->tv_sec = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}

/* ------------------------------------------------------------- startup */

/* Before main: files are binary (the disc images, the savestates; no CRLF
 * translation), the timer resolution is 1 ms for the waits that still go
 * through the scheduler's, and Winsock is up. */
__attribute__((constructor)) static void nc_win_init(void)
{
    _set_fmode(_O_BINARY);
    timeBeginPeriod(1);
    clock_init();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

/* ---------------------------------------------------------------- files */

ssize_t nc_pread(int fd, void *buf, size_t n, int64_t off)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD got = 0;
    if (n > 0x7FFFF000u) {
        n = 0x7FFFF000u;
    }
    if (!ReadFile(h, buf, (DWORD)n, &got, &ov)) {
        const DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) {
            return 0;
        }
        errno = e == ERROR_NOACCESS ? EFAULT : EIO;
        return -1;
    }
    return (ssize_t)got;
}

ssize_t nc_pwrite(int fd, const void *buf, size_t n, int64_t off)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD put = 0;
    if (n > 0x7FFFF000u) {
        n = 0x7FFFF000u;
    }
    if (!WriteFile(h, buf, (DWORD)n, &put, &ov)) {
        errno = GetLastError() == ERROR_DISK_FULL ? ENOSPC : EIO;
        return -1;
    }
    return (ssize_t)put;
}

/* A file's size set, grown or cut, with SetEndOfFile: mingw's ftruncate grows
 * a file by writing its zeros (half a second for 512 MiB on an SSD, seconds
 * on a USB hard disk: Forza Motorsport sizes its cache files before filling
 * them, and the console stood still up to 23 s at a time). NTFS gives the
 * grown part back as zeros all the same. The file pointer stays where it was. */
int nc_ftruncate(int fd, int64_t len)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE || len < 0) {
        errno = h == INVALID_HANDLE_VALUE ? EBADF : EINVAL;
        return -1;
    }
    LARGE_INTEGER zero = {0}, cur, to;
    to.QuadPart = len;
    if (!SetFilePointerEx(h, zero, &cur, FILE_CURRENT)) {
        errno = EIO;
        return -1;
    }
    const bool ok = SetFilePointerEx(h, to, NULL, FILE_BEGIN) && SetEndOfFile(h);
    const DWORD err = ok ? 0 : GetLastError();
    SetFilePointerEx(h, cur, NULL, FILE_BEGIN);
    if (!ok) {
        errno = err == ERROR_DISK_FULL ? ENOSPC : EIO;
        return -1;
    }
    return 0;
}

int statvfs(const char *path, struct statvfs *buf)
{
    ULARGE_INTEGER avail, total, free_;
    if (!GetDiskFreeSpaceExA(path, &avail, &total, &free_)) {
        errno = ENOENT;
        return -1;
    }
    buf->f_bsize = buf->f_frsize = 4096;
    buf->f_blocks = total.QuadPart / 4096;
    buf->f_bfree = free_.QuadPart / 4096;
    buf->f_bavail = avail.QuadPart / 4096;
    return 0;
}

/* ---------------------------------------------------------- environment */

int nc_setenv(const char *name, const char *value, int overwrite)
{
    if (!overwrite && getenv(name)) {
        return 0;
    }
    return _putenv_s(name, value) ? -1 : 0;
}

int nc_unsetenv(const char *name)
{
    return _putenv_s(name, "") ? -1 : 0;
}

int nc_gettid(void)
{
    return (int)GetCurrentThreadId();
}

void nc_win_thread_priority(int level)
{
    SetThreadPriority(GetCurrentThread(), level < 0    ? THREAD_PRIORITY_BELOW_NORMAL
                                          : level == 0 ? THREAD_PRIORITY_NORMAL
                                          : level == 1 ? THREAD_PRIORITY_ABOVE_NORMAL
                                                       : THREAD_PRIORITY_HIGHEST);
}

long nc_sysconf(int name)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    switch (name) {
    case _SC_PAGESIZE:
        return (long)si.dwPageSize;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN:
        return (long)si.dwNumberOfProcessors;
    default:
        errno = EINVAL;
        return -1;
    }
}

int nc_posix_memalign(void **p, size_t align, size_t size)
{
    *p = _aligned_malloc(size, align);
    return *p ? 0 : ENOMEM;
}

void nc_arc4random_buf(void *buf, size_t n)
{
    BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

uint32_t nc_arc4random(void)
{
    uint32_t v;
    nc_arc4random_buf(&v, sizeof(v));
    return v;
}

uint32_t nc_arc4random_uniform(uint32_t bound)
{
    if (bound < 2) {
        return 0;
    }
    const uint32_t min = -bound % bound; /* rejection: no modulo bias */
    for (;;) {
        const uint32_t r = nc_arc4random();
        if (r >= min) {
            return r % bound;
        }
    }
}

size_t nc_strlcpy(char *dst, const char *src, size_t size)
{
    const size_t n = strlen(src);
    if (size) {
        const size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

size_t nc_strlcat(char *dst, const char *src, size_t size)
{
    const size_t d = strnlen(dst, size);
    if (d == size) {
        return size + strlen(src);
    }
    return d + nc_strlcpy(dst + d, src, size - d);
}

static void thread_describe(HANDLE h, const char *name)
{
    typedef HRESULT(WINAPI * SetDesc)(HANDLE, PCWSTR);
    static SetDesc set;
    static bool looked;
    if (!looked) {
        set = (SetDesc)(void *)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "SetThreadDescription");
        looked = true;
    }
    wchar_t w[64];
    if (set && MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64) > 0) {
        set(h, w);
    }
}

int nc_pthread_setname_np(pthread_t t, const char *name)
{
    HANDLE h = pthread_gethandle(t);
    if (h) {
        thread_describe(h, name);
    }
    return 0;
}

void nc_win_thread_name(const char *name)
{
    thread_describe(GetCurrentThread(), name);
}

int nc_open(const char *path, int flags, ...)
{
    const int acc = flags & (O_RDONLY | O_WRONLY | O_RDWR);
    const DWORD access = acc == O_WRONLY ? GENERIC_WRITE : acc == O_RDWR ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    const DWORD disp = flags & O_CREAT ? (flags & O_EXCL ? CREATE_NEW : flags & O_TRUNC ? CREATE_ALWAYS : OPEN_ALWAYS)
                       : flags & O_TRUNC ? TRUNCATE_EXISTING
                                         : OPEN_EXISTING;
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME ? ENOENT
                : e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS                             ? EEXIST
                : e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION                        ? EACCES
                : e == ERROR_DISK_FULL                                                            ? ENOSPC
                                                                                                  : EIO;
        return -1;
    }
    const int fd = _open_osfhandle((intptr_t)h, (acc == O_RDONLY ? _O_RDONLY : 0) | (flags & O_APPEND ? _O_APPEND : 0));
    if (fd < 0) {
        CloseHandle(h);
    }
    return fd;
}

/* A file or folder opened only to be renamed or deleted, beside its other
 * handles (all of them share delete: nc_open). */
static HANDLE open_for_delete(const char *path)
{
    return CreateFileA(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
}

/* Windows 10's POSIX rename (FileRenameInfoEx): the name moves now, over an
 * existing file even if it is open, as rename(2) does. */
static bool posix_rename(const char *from, const char *to)
{
    WCHAR wto[MAX_PATH * 4];
    const int n = MultiByteToWideChar(CP_ACP, 0, to, -1, wto, MAX_PATH * 4);
    HANDLE h = n > 0 ? open_for_delete(from) : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    WCHAR full[MAX_PATH * 4];
    const DWORD len = GetFullPathNameW(wto, MAX_PATH * 4, full, NULL);
    bool ok = false;
    if (len && len < MAX_PATH * 4) {
        const size_t size = sizeof(FILE_RENAME_INFO) + len * sizeof(WCHAR);
        FILE_RENAME_INFO *ri = calloc(1, size);
        if (ri) {
            ri->Flags = 0x1 | 0x2; /* FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS */
            ri->FileNameLength = len * sizeof(WCHAR);
            memcpy(ri->FileName, full, len * sizeof(WCHAR));
            ok = SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS)22 /* FileRenameInfoEx */, ri, (DWORD)size);
            free(ri);
        }
    }
    CloseHandle(h);
    return ok;
}

int nc_unlink(const char *path)
{
    /* POSIX semantics (FileDispositionInfoEx): the name goes now, even while
     * the file is open, so a new file can take it */
    HANDLE h = open_for_delete(path);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD flags = 0x1 | 0x2; /* FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS */
        const bool ok = SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS)21 /* FileDispositionInfoEx */,
                                                   &flags, sizeof(flags));
        CloseHandle(h);
        if (ok) {
            return 0;
        }
    }
    if (DeleteFileA(path)) {
        return 0;
    }
    const DWORD e = GetLastError();
    errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
    return -1;
}

int nc_rename(const char *from, const char *to)
{
    if (posix_rename(from, to) || MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        return 0;
    }
    const DWORD e = GetLastError();
    errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT
            : e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION ? EACCES
            : e == ERROR_ALREADY_EXISTS || e == ERROR_DIR_NOT_EMPTY ? EEXIST
                                                                   : EIO;
    return -1;
}

void *nc_memmem(const void *hay, size_t hlen, const void *needle, size_t nlen)
{
    if (!nlen) {
        return (void *)hay;
    }
    const uint8_t *h = hay, *n = needle;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (h[i] == n[0] && !memcmp(h + i, n, nlen)) {
            return (void *)(h + i);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ file times */

static void unix_to_filetime(const struct timespec *t, FILETIME *ft)
{
    const uint64_t v = (uint64_t)t->tv_sec * 10000000ull + (uint64_t)t->tv_nsec / 100 + 116444736000000000ull;
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

static int set_times(HANDLE h, const struct timespec ts[2])
{
    FILETIME a, m, now;
    GetSystemTimeAsFileTime(&now);
    const FILETIME *pa = NULL, *pm = NULL;
    if (ts) {
        if (ts[0].tv_nsec != UTIME_OMIT) {
            if (ts[0].tv_nsec == UTIME_NOW) {
                a = now;
            } else {
                unix_to_filetime(&ts[0], &a);
            }
            pa = &a;
        }
        if (ts[1].tv_nsec != UTIME_OMIT) {
            if (ts[1].tv_nsec == UTIME_NOW) {
                m = now;
            } else {
                unix_to_filetime(&ts[1], &m);
            }
            pm = &m;
        }
    } else {
        pa = pm = &now;
    }
    if (!SetFileTime(h, NULL, pa, pm)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int nc_utimensat(int dirfd, const char *path, const struct timespec ts[2], int flags)
{
    HANDLE h = CreateFileA(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        errno = ENOENT;
        return -1;
    }
    const int r = set_times(h, ts);
    CloseHandle(h);
    return r;
}

int nc_futimens(int fd, const struct timespec ts[2])
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    return set_times(h, ts);
}

int nc_stat_ns(const char *path, uint64_t *mtime_ns, uint64_t *ctime_ns)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
        errno = ENOENT;
        return -1;
    }
    const uint64_t w = ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime);
    const uint64_t ns = w >= 116444736000000000ull ? (w - 116444736000000000ull) * 100 : 0;
    if (mtime_ns) {
        *mtime_ns = ns;
    }
    if (ctime_ns) {
        *ctime_ns = ns;
    }
    return 0;
}

/* --------------------------------------------------------------- sleeps */

/* One high-resolution waitable timer per thread (Windows 10 1803 up): waits
 * end within a few tens of microseconds of their time, where Sleep rounds to
 * the 1 ms tick at best. */
static HANDLE thread_timer(void)
{
    static __thread HANDLE t;
    if (!t) {
        t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t) {
            t = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        }
    }
    return t;
}

static void sleep_100ns(int64_t ticks)
{
    if (ticks <= 0) {
        SwitchToThread();
        return;
    }
    HANDLE t = thread_timer();
    LARGE_INTEGER due;
    due.QuadPart = -ticks;
    if (t && SetWaitableTimerEx(t, &due, 0, NULL, NULL, NULL, 0)) {
        WaitForSingleObject(t, INFINITE);
    } else {
        Sleep((DWORD)((ticks + 9999) / 10000));
    }
}

int nc_nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    sleep_100ns((int64_t)req->tv_sec * 10000000 + req->tv_nsec / 100);
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

int nc_usleep(unsigned long long usec)
{
    sleep_100ns((int64_t)usec * 10);
    return 0;
}

static int64_t ts_ns(const struct timespec *t)
{
    return (int64_t)t->tv_sec * 1000000000LL + t->tv_nsec;
}

int nc_clock_nanosleep(clockid_t clk, int flags, const struct timespec *req, struct timespec *rem)
{
    int64_t ns = ts_ns(req);
    if (flags & 1 /* TIMER_ABSTIME */) {
        struct timespec now;
        clock_gettime(clk, &now);
        ns -= ts_ns(&now);
    }
    sleep_100ns(ns > 0 ? ns / 100 : 0);
    return 0;
}

/* The deadline's clock by its size: a CLOCK_REALTIME deadline is decades
 * of seconds since 1970, a CLOCK_MONOTONIC one the time since boot. */
int nc_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)
{
    struct timespec now, rel;
    clock_gettime(abstime->tv_sec > 1000000000 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
    int64_t left = ts_ns(abstime) - ts_ns(&now);
    if (left < 0) {
        left = 0;
    }
    rel.tv_sec = (time_t)(left / 1000000000LL);
    rel.tv_nsec = (long)(left % 1000000000LL);
    return pthread_cond_timedwait_relative_np(c, m, &rel);
}

/* ----------------------------------------------------------- properties */

/* props.ini beside the executable: name=value lines ('#' comments). Read
 * again when its time changes, looked at no more than every 100 ms. */
static SRWLOCK g_props_lock = SRWLOCK_INIT;
static char *g_props;           /* the file's text, lines NUL-separated */
static size_t g_props_len;
static FILETIME g_props_time;
static ULONGLONG g_props_checked;

static void props_path(char *out, size_t n)
{
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof(exe));
    while (len && exe[len - 1] != '\\' && exe[len - 1] != '/') {
        len--;
    }
    exe[len] = 0;
    snprintf(out, n, "%sprops.ini", exe);
}

static void props_refresh(void)
{
    const ULONGLONG now = GetTickCount64();
    if (g_props_checked && now - g_props_checked < 100) {
        return;
    }
    g_props_checked = now;
    char path[MAX_PATH + 16];
    props_path(path, sizeof(path));
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
        free(g_props);
        g_props = NULL;
        g_props_len = 0;
        return;
    }
    if (g_props && !CompareFileTime(&fa.ftLastWriteTime, &g_props_time)) {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    char *buf = malloc(64 * 1024);
    size_t n = buf ? fread(buf, 1, 64 * 1024 - 1, f) : 0;
    fclose(f);
    if (!buf) {
        return;
    }
    buf[n] = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\r' || buf[i] == '\n') {
            buf[i] = 0;
        }
    }
    free(g_props);
    g_props = buf;
    g_props_len = n;
    g_props_time = fa.ftLastWriteTime;
}

/* values the front end sets while running (fast forward's debug.hakux.uncap),
 * over the file's */
static struct {
    char name[PROP_NAME_MAX];
    char value[PROP_VALUE_MAX];
} g_prop_set[16];

void nc_win_prop_set(const char *name, const char *value)
{
    AcquireSRWLockExclusive(&g_props_lock);
    int free_slot = -1;
    for (int i = 0; i < 16; i++) {
        if (!strcmp(g_prop_set[i].name, name)) {
            free_slot = i;
            break;
        }
        if (!g_prop_set[i].name[0] && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot >= 0) {
        snprintf(g_prop_set[free_slot].name, PROP_NAME_MAX, "%s", value ? name : "");
        snprintf(g_prop_set[free_slot].value, PROP_VALUE_MAX, "%s", value ? value : "");
    }
    ReleaseSRWLockExclusive(&g_props_lock);
}

int __system_property_get(const char *name, char *value)
{
    value[0] = 0;
    AcquireSRWLockExclusive(&g_props_lock);
    for (int i = 0; i < 16; i++) {
        if (g_prop_set[i].name[0] && !strcmp(g_prop_set[i].name, name)) {
            snprintf(value, PROP_VALUE_MAX, "%s", g_prop_set[i].value);
            ReleaseSRWLockExclusive(&g_props_lock);
            return (int)strlen(value);
        }
    }
    props_refresh();
    const size_t nl = strlen(name);
    for (size_t i = 0; g_props && i < g_props_len;) {
        const char *line = g_props + i;
        const size_t ll = strlen(line);
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (line[0] != '#' && !strncmp(line, name, nl)) {
            const char *p = line + nl;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '=') {
                p++;
                while (*p == ' ' || *p == '\t') {
                    p++;
                }
                snprintf(value, PROP_VALUE_MAX, "%s", p);
                size_t vl = strlen(value);
                while (vl && (value[vl - 1] == ' ' || value[vl - 1] == '\t')) {
                    value[--vl] = 0;
                }
                ReleaseSRWLockExclusive(&g_props_lock);
                return (int)vl;
            }
        }
        i += ll + 1;
    }
    ReleaseSRWLockExclusive(&g_props_lock);
    /* the environment: DEBUG_HAKUX_PERF for debug.hakux.perf */
    char env[PROP_NAME_MAX * 2 + 8];
    size_t k = 0;
    for (; name[k] && k < sizeof(env) - 1; k++) {
        const char c = name[k];
        env[k] = c == '.' ? '_' : (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    env[k] = 0;
    const char *e = getenv(env);
    if (e) {
        snprintf(value, PROP_VALUE_MAX, "%s", e);
        return (int)strlen(value);
    }
    return 0;
}

/* --------------------------------------------------------------- memory */

static DWORD win_prot(int prot)
{
    if (prot & PROT_EXEC) {
        return prot & PROT_WRITE ? PAGE_EXECUTE_READWRITE : prot & PROT_READ ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    }
    return prot & PROT_WRITE ? PAGE_READWRITE : prot & PROT_READ ? PAGE_READONLY : PAGE_NOACCESS;
}

void *nc_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off)
{
    if (flags & MAP_ANONYMOUS) {
        void *p = VirtualAlloc((flags & MAP_FIXED) ? addr : NULL, len, MEM_RESERVE | MEM_COMMIT, win_prot(prot));
        if (!p) {
            errno = ENOMEM;
            return MAP_FAILED;
        }
        return p;
    }
    HANDLE fh = (HANDLE)_get_osfhandle(fd);
    if (fh == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return MAP_FAILED;
    }
    const bool write = (prot & PROT_WRITE) != 0;
    const bool priv = (flags & MAP_PRIVATE) != 0;
    const uint64_t end = (uint64_t)off + len;
    HANDLE m = CreateFileMappingA(fh, NULL, write ? (priv ? PAGE_WRITECOPY : PAGE_READWRITE) : PAGE_READONLY,
                                  (DWORD)(end >> 32), (DWORD)end, NULL);
    if (!m) {
        errno = EACCES;
        return MAP_FAILED;
    }
    void *p = MapViewOfFileEx(m, write ? (priv ? FILE_MAP_COPY : FILE_MAP_WRITE) : FILE_MAP_READ,
                              (DWORD)((uint64_t)off >> 32), (DWORD)off, len, (flags & MAP_FIXED) ? addr : NULL);
    CloseHandle(m); /* the view keeps the mapping */
    if (!p) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return p;
}

int nc_munmap(void *addr, size_t len)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof(mbi))) {
        errno = EINVAL;
        return -1;
    }
    if (mbi.Type == MEM_MAPPED) {
        return UnmapViewOfFile(mbi.AllocationBase) ? 0 : -1;
    }
    return VirtualFree(mbi.AllocationBase, 0, MEM_RELEASE) ? 0 : -1;
}

int nc_mprotect(void *addr, size_t len, int prot)
{
    DWORD old;
    if (!VirtualProtect(addr, len, win_prot(prot), &old)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int nc_madvise(void *addr, size_t len, int advice)
{
    if (advice == MADV_DONTNEED) {
        /* the pages' contents may go: anonymous memory reads back as zeros */
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.Type == MEM_PRIVATE) {
            VirtualAlloc(addr, len, MEM_RESET, PAGE_NOACCESS);
        }
    }
    return 0;
}

int nc_msync(void *addr, size_t len, int flags)
{
    return FlushViewOfFile(addr, len) ? 0 : -1;
}

/* ----------------------------------------------------------- libraries */

static __thread char g_dlerr[128];

void *dlopen(const char *file, int mode)
{
    HMODULE m = file ? LoadLibraryA(file) : GetModuleHandleA(NULL);
    if (!m) {
        snprintf(g_dlerr, sizeof(g_dlerr), "LoadLibrary(%s): error %lu", file, GetLastError());
    }
    return m;
}

void *dlsym(void *handle, const char *name)
{
    void *p = (void *)GetProcAddress(handle ? (HMODULE)handle : GetModuleHandleA(NULL), name);
    if (!p) {
        snprintf(g_dlerr, sizeof(g_dlerr), "%s: not found", name);
    }
    return p;
}

int dlclose(void *handle)
{
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}

const char *dlerror(void)
{
    return g_dlerr[0] ? g_dlerr : NULL;
}

int dladdr(const void *addr, Dl_info *info)
{
    static __thread char name[MAX_PATH];
    HMODULE m;
    memset(info, 0, sizeof(*info));
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &m)) {
        return 0;
    }
    GetModuleFileNameA(m, name, sizeof(name));
    info->dli_fname = name;
    info->dli_fbase = m;
    return 1;
}

/* -------------------------------------------------------------- sockets */

int nc_sock_close(SOCKET s)
{
    return closesocket(s);
}

int nc_sock_nonblock(SOCKET s)
{
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on);
}

int nc_sock_poll(void *fds, unsigned long n, int timeout)
{
    return WSAPoll((WSAPOLLFD *)fds, (ULONG)n, timeout);
}

int nc_sock_errno(void)
{
    switch (WSAGetLastError()) {
    case WSAEWOULDBLOCK:
        return EAGAIN;
    case WSAEINPROGRESS:
        return EINPROGRESS;
    case WSAEINTR:
        return EINTR;
    case WSAECONNREFUSED:
        return ECONNREFUSED;
    case WSAECONNRESET:
        return ECONNRESET;
    case WSAEADDRINUSE:
        return EADDRINUSE;
    case WSAENOTCONN:
        return ENOTCONN;
    case WSAEMSGSIZE:
        return EMSGSIZE;
    case WSAETIMEDOUT:
        return ETIMEDOUT;
    default:
        return EIO;
    }
}

/* An eventfd as a loopback UDP socket that sends to itself. */
int nc_eventfd(void)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return -1;
    }
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(a);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || getsockname(s, (struct sockaddr *)&a, &len) != 0 ||
        connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        closesocket(s);
        return -1;
    }
    nc_sock_nonblock(s);
    return (int)s;
}

void nc_eventfd_write(int fd)
{
    const char one = 1;
    send((SOCKET)fd, &one, 1, 0);
}

void nc_eventfd_read(int fd)
{
    char buf[64];
    while (recv((SOCKET)fd, buf, sizeof(buf), 0) > 0) {
    }
}

/* The IPv4 addresses of the up interfaces. */
int getifaddrs(struct ifaddrs **list)
{
    *list = NULL;
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *aa = NULL;
    for (int tries = 0; tries < 3; tries++) {
        aa = malloc(size);
        if (!aa) {
            return -1;
        }
        const ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                                          GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &size);
        if (r == NO_ERROR) {
            break;
        }
        free(aa);
        aa = NULL;
        if (r != ERROR_BUFFER_OVERFLOW) {
            return -1;
        }
    }
    if (!aa) {
        return -1;
    }
    for (IP_ADAPTER_ADDRESSES *a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) {
            continue;
        }
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            struct {
                struct ifaddrs ifa;
                struct sockaddr_in addr, mask;
                char name[64];
            } *e = calloc(1, sizeof(*e));
            if (!e) {
                break;
            }
            memcpy(&e->addr, u->Address.lpSockaddr, sizeof(e->addr));
            e->mask.sin_family = AF_INET;
            const ULONG bits = u->OnLinkPrefixLength;
            e->mask.sin_addr.s_addr = bits ? htonl(0xFFFFFFFFu << (32 - (bits > 32 ? 32 : bits))) : 0;
            snprintf(e->name, sizeof(e->name), "%s", a->AdapterName);
            e->ifa.ifa_name = e->name;
            e->ifa.ifa_addr = (struct sockaddr *)&e->addr;
            e->ifa.ifa_netmask = (struct sockaddr *)&e->mask;
            e->ifa.ifa_flags = 1; /* IFF_UP */
            e->ifa.ifa_next = *list;
            *list = &e->ifa;
        }
    }
    free(aa);
    return 0;
}

void freeifaddrs(struct ifaddrs *list)
{
    while (list) {
        struct ifaddrs *n = list->ifa_next;
        free(list);
        list = n;
    }
}
