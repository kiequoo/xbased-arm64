/* xbased on Windows: statvfs on GetDiskFreeSpaceEx (compat/win/win.c). */
#ifndef NC_WIN_STATVFS_H
#define NC_WIN_STATVFS_H
typedef unsigned long long fsblkcnt_t;
struct statvfs {
    unsigned long f_bsize, f_frsize;
    fsblkcnt_t f_blocks, f_bfree, f_bavail;
};
#ifdef __cplusplus
extern "C" {
#endif
int statvfs(const char *path, struct statvfs *buf);
#ifdef __cplusplus
}
#endif
#endif
