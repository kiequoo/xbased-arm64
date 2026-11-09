/*
 * xbased — the game's frame: one top-level window for a whole game
 * session, in a small process of its own, with each run of the emulator's
 * window inside it as a child.
 *
 * A run that ends for another (a title launching another executable or
 * rebooting, a state loaded, a restart: run_again) used to swap one
 * top-level window for another: a new taskbar button, the focus moving,
 * a full screen mode set again. Inside the frame the next run's window
 * comes up under this one's, draws, goes on top and this one goes: to the
 * desktop and to the player the window never changed.
 *
 * Windows: "xbased.exe frame ...", the run's window a child window of the
 * frame's. Linux: X11 only (a Wayland client can't hold another's
 * surface): the game's process forks before anything else, the parent
 * (the process the dashboard waits on) becomes the frame, the run's
 * window reparented into it.
 */
#ifndef XBASED_WIN_FRAME_H
#define XBASED_WIN_FRAME_H

#include <stdbool.h>

struct SDL_Window;

#ifdef _WIN32
/* "xbased.exe frame <token> <full screen 0|1> <fsrate60 0|1> <title>":
 * the frame's own process; returns its exit code. */
int nc_frame_main(int argc, char **argv);
#else
/* Before SDL starts: the process forks, the parent stays as the frame
 * (never returns: its exit ends the session for the dashboard), the child
 * goes on as the run with XBASED_FRAME_XID set; nothing (no X11, Wayland:
 * no frame) and the run goes on as before. */
void nc_frame_fork(bool fullscreen, bool fs60);
#endif

/* The game's process: the frame this session draws in (a relaunched run's,
 * or the one just started for it); NULL if none (the run's window then
 * stands on its own, as before). */
void *nc_frame_get(const char *title, bool fullscreen, bool fs60);

/* The run's window made a child of the frame's, filling it, under any
 * child already there (the run before's, until this one has drawn); the
 * frame takes this run's title. */
void nc_frame_adopt(void *frame, struct SDL_Window *child, const char *title, bool hidden);
/* The run's window filling the frame (it presents to it itself: the frame's
 * pictures couldn't be had). */
void nc_frame_fill(void *frame, struct SDL_Window *child);
/* The frame's own pictures (hw/xbox/hle/vk/share.h): its block's token. */
const char *nc_frame_token(void);
/* The run's window on top of the frame's children, with the keyboard. */
void nc_frame_raise(void *frame, struct SDL_Window *child);
/* Full screen (borderless, the panel's mode a multiple of 60 Hz if the
 * frame was asked for it) or a window, in turn. */
void nc_frame_toggle_fullscreen(void *frame);
bool nc_frame_minimized(void *frame);
/* The session's over (back to the dashboard): the frame goes at once. */
void nc_frame_close(void *frame);

#endif
