/*
 * xbased (xbased for PC) — the dev tools' control of a run (dev builds only:
 * XBASED_DEVCTL; a local tool drives it). Not in a release.
 *
 * A socket on 127.0.0.1 (XBASED_DEVCTL_PORT, 47860 by default; with
 * XBASED_DEVCTL_TOKEN set, a connection's first line is "auth <token>"): one
 * command a line, one JSON object a line back. The window's thread runs what
 * touches the window, the input and the pause (nc_devctl_tick); the guest's
 * CPU thread at each vblank the memory writes and the frozen values
 * (nc_mods_vblank_hook); the rest runs on the server's thread.
 *
 *   ping | status | quit
 *   shot <path>                    a picture of the next refresh (BMP)
 *   act <action>                   win_auto.h's actions: buttons, sticks,
 *                                  save N, load N, menu, prof N, vpad ...
 *   pause | resume | step <frames> the emulation held, let go, or let go for
 *                                  that many frames (the back end's swaps)
 *   mem read <va> <len>            hex; "pread" by physical address
 *   mem write <va> <hex>           at the next vblank (at once when paused)
 *   mem find <hex> [max]           the addresses holding those bytes
 *   watch add <va> <len> | watch get | watch clear
 *                                  a value's changes, sampled each millisecond
 *   freeze <va> <hex> | unfreeze   written again at every vblank
 *   threads                        the guest's threads: state, registers (whole
 *                                  for the running one while paused), stack
 *   disasm <va> <count>            the guest's code there
 *   prop get <name> | prop set <name> <value>
 *                                  the debug properties (debug.xbased.*, live)
 *   capture <path> [detail]        the next frame's draws (vk/devcap.h)
 *   skip <spec>                    draws of each frame left out (devcap.h)
 *   stats                          the draw path's counters
 *   frames <since>                 the per-frame record from that row (devcap.h)
 *   mouse <dx> <dy> <buttons> <held>
 *                                  the mouse as held for a game's stick
 *                                  (#255), moved and its buttons (SDL's
 *                                  mask), without taking the real one
 */
#ifndef NATIVE_APP_WIN_DEVCTL_H
#define NATIVE_APP_WIN_DEVCTL_H

#ifdef XBASED_DEVCTL
#include <stdbool.h>
#include "native/core/core.h"

/* At the window's start: the server up (data_dir: the log's folder) */
void nc_devctl_init(const char *data_dir, NcCore *core);
/* Each pass of the window's loop: the window thread's commands; *paused is
 * the loop's own (the hotkey's), kept in step */
void nc_devctl_tick(bool *paused, bool minimized);
/* (win_main.c) the "mouse" command's: held 1 or 0 (-1 as it is), its move
 * and buttons, as a held mouse's */
void nc_win_dev_mouse(float dx, float dy, uint32_t buttons, int held);
#endif

#endif
