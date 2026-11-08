/*
 * hakuX on Windows — the POSIX the core uses that MinGW-w64 doesn't have.
 *
 * Included first in every translation unit of the Windows build (win.cmake,
 * -include). It doesn't pull in <windows.h>: the core's names (ERROR, OUT,
 * near...) would meet its macros. The functions live in compat/win/win.c.
 */
#ifndef NC_WIN_POSIX_H
#define NC_WIN_POSIX_H

#ifdef _WIN32

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64 /* off_t, lseek, ftruncate: disc images are 7 GiB */
#endif
#ifndef _POSIX_THREAD_SAFE_FUNCTIONS
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L /* gmtime_r, localtime_r, strtok_r */
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* the CRT's own declarations first: the macros below rename some */
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

/* the kernel's coarse clock: ~1 ms steps, read from memory (nc_clock_gettime) */
#define CLOCK_MONOTONIC_COARSE 6
#define CLOCK_BOOTTIME CLOCK_MONOTONIC
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

ssize_t nc_pread(int fd, void *buf, size_t n, int64_t off);
ssize_t nc_pwrite(int fd, const void *buf, size_t n, int64_t off);
#define pread nc_pread
#define pwrite nc_pwrite
/* SetEndOfFile, not mingw's zero-writing ftruncate (win.c) */
int nc_ftruncate(int fd, int64_t len);
#define ftruncate(fd, len) nc_ftruncate((fd), (int64_t)(len))

int nc_setenv(const char *name, const char *value, int overwrite);
int nc_unsetenv(const char *name);
#define setenv nc_setenv
#define unsetenv nc_unsetenv

int nc_gettid(void);
/* The calling thread's priority: -1 below normal, 0 normal, 1 above, 2 the
 * highest a normal process gets (THREAD_PRIORITY_HIGHEST). */
void nc_win_thread_priority(int level);
#define gettid nc_gettid

#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
long nc_sysconf(int name);
#define sysconf nc_sysconf

/* memory from it is never freed in the core (the ring) */
int nc_posix_memalign(void **p, size_t align, size_t size);
#define posix_memalign nc_posix_memalign

void nc_arc4random_buf(void *buf, size_t n);
uint32_t nc_arc4random(void);
uint32_t nc_arc4random_uniform(uint32_t bound);
#define arc4random_buf nc_arc4random_buf
#define arc4random nc_arc4random
#define arc4random_uniform nc_arc4random_uniform

size_t nc_strlcpy(char *dst, const char *src, size_t size);
size_t nc_strlcat(char *dst, const char *src, size_t size);
#define strlcpy nc_strlcpy
#define strlcat nc_strlcat

#define fsync _commit
#define fdatasync _commit
#define lstat stat
/* POSIX's mkdir(path, mode) */
#define mkdir(path, mode) _mkdir(path)

/* Thread names as Windows keeps them (SetThreadDescription: debuggers,
 * profilers, the vCPU profile); winpthreads' only told a debugger. */
int nc_pthread_setname_np(pthread_t t, const char *name);
void nc_win_thread_name(const char *name);  /* the calling thread's */
#define pthread_setname_np nc_pthread_setname_np

/* POSIX's open: a file open here can be renamed or deleted meanwhile, as on
 * Linux (the CRT's leaves FILE_SHARE_DELETE out). A title's cache copy
 * renamed its .tmp file while open, and every rename failed: Panzer Dragoon
 * Orta then showed its dirty-disc message. Binary, always. */
int nc_open(const char *path, int flags, ...);
#define open(...) nc_open(__VA_ARGS__)
/* POSIX's unlink: the name goes now, even while the file is open */
int nc_unlink(const char *path);
#define unlink nc_unlink

/* POSIX's rename: an existing file at to is replaced (the CRT's fails) */
int nc_rename(const char *from, const char *to);
#define rename nc_rename

void *nc_memmem(const void *hay, size_t hlen, const void *needle, size_t nlen);
#define memmem nc_memmem

/* File times (utimensat, futimens) and their 100 ns readings (struct stat
 * has whole seconds here: nc_stat_ns gives the write time as Linux's
 * st_mtim and st_ctim would). */
#define UTIME_NOW ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
#define AT_FDCWD (-100)
struct timespec;
int nc_utimensat(int dirfd, const char *path, const struct timespec ts[2], int flags);
int nc_futimens(int fd, const struct timespec ts[2]);
#define utimensat nc_utimensat
#define futimens nc_futimens
int nc_stat_ns(const char *path, uint64_t *mtime_ns, uint64_t *ctime_ns);

/* CLOCK_MONOTONIC from the invariant TSC, scaled (a read of a register, as
 * Linux's vDSO gives it); QueryPerformanceCounter otherwise. The kernel
 * reads the time at every call: winpthreads' clock_gettime through QPC was
 * a tenth of the guest CPU's thread. */
int nc_clock_gettime(clockid_t clk, struct timespec *ts);
#undef clock_gettime
#define clock_gettime nc_clock_gettime

/* Timed sleeps at the host timer's resolution (high-resolution waitable
 * timers), not the scheduler tick's: the guest's short waits count on it. */
struct timespec;
int nc_nanosleep(const struct timespec *req, struct timespec *rem);
int nc_usleep(unsigned long long usec);
#define nanosleep nc_nanosleep
#define usleep nc_usleep
/* Absolute waits on CLOCK_MONOTONIC (the core's condition variables are
 * set to it), on a high-resolution timer for the sleeps. */
int nc_clock_nanosleep(clockid_t clk, int flags, const struct timespec *req, struct timespec *rem);
int nc_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime);
#undef clock_nanosleep
#define clock_nanosleep nc_clock_nanosleep
#define pthread_cond_timedwait nc_cond_timedwait

/* Signals the core names; the faults themselves come through a vectored
 * exception handler (compat/win/win.c). */
#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SIGPROF
#define SIGPROF 27
#endif
#ifndef SIGUSR1
#define SIGUSR1 10
#endif
#ifndef SIGUSR2
#define SIGUSR2 12
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
typedef struct nc_siginfo {
    int si_signo;
    int si_code;
    void *si_addr;
    int nc_write; /* the access was a write */
} siginfo_t;
typedef unsigned long nc_sigset_t;
#define sigset_t nc_sigset_t
static inline int nc_sigemptyset(sigset_t *s) { *s = 0; return 0; }
static inline int nc_sigfillset(sigset_t *s) { *s = ~0ul; return 0; }
static inline int nc_sigaddset(sigset_t *s, int n) { *s |= 1ul << (n & 31); return 0; }
static inline int nc_sigdelset(sigset_t *s, int n) { *s &= ~(1ul << (n & 31)); return 0; }
#define sigemptyset nc_sigemptyset
#define sigfillset nc_sigfillset
#define sigaddset nc_sigaddset
#define sigdelset nc_sigdelset
#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif
static inline int nc_sigmask(int how, const sigset_t *s, sigset_t *old)
{
    (void)how;
    (void)s;
    if (old) {
        *old = 0;
    }
    return 0;
}
#undef pthread_sigmask
#define pthread_sigmask nc_sigmask
#define sigprocmask nc_sigmask

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif
