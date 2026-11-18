/*
 * xbased front end — the mouse pointer: xbased's own, a green dart in the
 * dashboard's colours (a light top, the console's green below, a dark edge
 * and a soft glow), drawn here at the display's scale rather than an image
 * scaled up. The dashboard and the game's window (SDL) take it, and the
 * session's frame on Windows (win_frame.c) as a cursor of its own.
 */
#ifndef XBASED_POINTER_H
#define XBASED_POINTER_H

#include <SDL3/SDL.h>
#include <stdint.h>

/* The pointer's picture for a scale (1 at 96 dpi): *size by *size pixels,
 * 0xAARRGGBB, straight alpha, in a buffer of the caller's (ptr_art_size's
 * pixels at least); its tip, the hot spot, at (*hot, *hot). */
int ptr_art_size(float scale);
void ptr_art(float scale, uint32_t *argb, int *size, int *hot);

/* The pointer set for SDL's windows in this process (made once, at the
 * window's display scale); shown or hidden as before. */
void ptr_install(SDL_Window *win);

#endif
