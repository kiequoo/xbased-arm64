/*
 * xbased (xbased for PC) — the run's mods and cheats (win_mods.c).
 */
#ifndef NATIVE_APP_WIN_MODS_H
#define NATIVE_APP_WIN_MODS_H

#include "native/core/nc.h"

/* The manifest of the disc's mods (exe_dir\mods\<TITLE ID>.<version>), the
 * ones opts name on ("mod=<id>", "cheat=<name>"), handed to the core. False
 * when the disc has none. Before boot. */
bool nc_win_mods_prepare(const char *exe_dir, int disc_fd, char **opts, int nopts);
/* The database patches a newer package took away (exe_dir\mods\removed.txt)
 * deleted where they're still the database's copy; once a run. */
void nc_win_mods_cleanup(const char *exe_dir);
/* An IPS patch (dir/<id>.ips) offered for the disc: the user's always; the
 * database's (exe_dir\mods\<title id>, mods\patches.txt) only on a North
 * American executable whose code it fits (mod_fit.h). why: if not. */
bool nc_win_mods_ips_fits(const char *exe_dir, int disc_fd, const char *dir, const char *id, char *why, size_t wn);
/* A .jmp patch (path) for the disc's executable (mod_jmp.h: why not, and
 * its header's notes and author into info) */
struct JmpInfo;
bool nc_win_mods_jmp_fits(int disc_fd, const char *path, struct JmpInfo *info, char *why, size_t wn);

#endif
