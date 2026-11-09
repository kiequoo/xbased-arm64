/*
 * hakuX front end — the dashboard (dash.c): the console-style menus
 * before a game: the library, settings, saves, online. Runs until the user
 * leaves; each game it starts runs in a process of its own.
 */
#ifndef NATIVE_APP_UI_DASH_H
#define NATIVE_APP_UI_DASH_H

#include <SDL3/SDL.h>

/* The dashboard in win until the user leaves. exe: this executable (the
 * games' process). */
int dash_run(SDL_Window *win, const char *exe);

/* The in-game menu over the paused game in win: with the interface up as
 * the game's overlay (ui_init_overlay), drawn by the game's presenter over
 * its picture, blurred; otherwise (frame_bmp: its last frame) on a renderer
 * of its own in win, whose presenter let it go. What the player chose, and
 * for a state the slot. Settings changed are saved to the files; live (may
 * be NULL), each frame, has the ones being changed, to show at once. */
enum { DASH_INGAME_RESUME, DASH_INGAME_SAVE, DASH_INGAME_LOAD, DASH_INGAME_RESTART, DASH_INGAME_QUIT };
struct Config;
/* Full screen in the in-game menu (F11, Alt+Enter): this instead of the
 * window's own, when the game's window is inside a frame (win_frame.h) */
void dash_set_fullscreen_hook(void (*toggle)(void));
/* The pointer's place (window pixels) from elsewhere (the game's frame):
 * false, none; NULL, SDL's own. */
void dash_set_pointer_hook(bool (*where)(float *x, float *y));
int dash_ingame(SDL_Window *win, const char *key, const char *title, const char *frame_bmp, int *slot,
                void (*live)(const struct Config *global, const struct Config *game));

#endif
