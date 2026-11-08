/*
 * xbased — the guest's memory and Windows' address space randomization.
 *
 * The guest's 4 GiB are the low 4 GiB of the process (core/vm_win.cpp), so
 * the process keeps its own memory above them: bottom-up and high-entropy
 * ASLR do that. With bottom-up ASLR turned off (Windows Security, Exploit
 * protection, for the whole system), Windows puts the process's first
 * memory at the bottom of the address space, where every XBE goes
 * (0x10000), before any of xbased's code runs. So xbased's processes start
 * their games with both turned on for those processes alone (a process
 * creation attribute, no setting changed), and a process started without
 * them, finding its low memory taken, starts itself again that way.
 */
#ifndef XBASED_WIN_ASLR_H
#define XBASED_WIN_ASLR_H

#ifdef _WIN32
#include <stdbool.h>
#include <windows.h>

/* CreateProcessA (the process code page is UTF-8) with bottom-up and
 * high-entropy ASLR on for the new process; without the attribute where
 * Windows doesn't take it. */
bool xb_spawn(const char *exe, char *cmd, const char *dir, PROCESS_INFORMATION *pi);

/* At main's start: the low 4 GiB taken by the host and this process without
 * bottom-up ASLR, it runs itself again (xb_spawn), waits and ends with that
 * run's exit code. Otherwise it returns. */
void xb_aslr_check(void);

/* The low 4 GiB hold host memory beyond Windows' shared pages (a game can't
 * boot): for the message the boot's failure shows. */
bool xb_aslr_low_taken(void);
#endif

#endif
