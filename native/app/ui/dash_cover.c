/*
 * xbased front end — the cover picker (dash_int.h): box art for a game from
 * libretro-thumbnails' list (library.c lib_cover_candidates), the names that
 * fit the game first and a search for the others (the on-screen keyboard's,
 * dash_find.c), the selected name's picture fetched while it is the one
 * looked at; or a picture file of the user's. A title two editions share
 * (Dead or Alive Ultimate's two discs) or a name the list spells another way
 * had the wrong cover or none, and a file was the only way to change it
 * (#318).
 */
#include "native/app/ui/dash_int.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define ROWS 40             /* names shown */

static struct {
    LibGame *g;
    char q[64];             /* the search ("": the names that fit the game) */
    bool editing;           /* typing it */
} C;

static void edit(bool on)
{
    C.editing = on;
    D.typing = on;
    if (on) {
        SDL_StartTextInput(SDL_GetKeyboardFocus());
    } else {
        SDL_StopTextInput(SDL_GetKeyboardFocus());
    }
}

void dash_cover_open(LibGame *g)
{
    C.g = g;
    C.q[0] = 0;
    lib_cover_picker_open();
    D.sel[SCR_COVER] = 1;       /* the best name (row 0 is the search) */
    D.scroll[SCR_COVER] = 0;
    dash_push(SCR_COVER);
    edit(false);
}

/* a name as its row shows it: without ".png" */
static void row_name(const char *nm, char *out, size_t n)
{
    snprintf(out, n, "%s", nm);
    const size_t l = strlen(out);
    if (l > 4 && !strcmp(out + l - 4, ".png")) {
        out[l - 4] = 0;
    }
}

/* a picture in the box (x, y, w, h), as big as it fits, its shape kept */
static void fit_image(UiTex *t, float x, float y, float w, float h)
{
    float tw = 1, th = 1;
    ui_texture_size(t, &tw, &th);
    if (tw <= 0 || th <= 0) {
        return;
    }
    const float s = fminf(w / tw, h / th), dw = tw * s, dh = th * s;
    ui_image(t, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, 1.0f);
}

void scr_cover(void)
{
    LibGame *g = C.g;
    if (!g) {
        dash_pop();
        return;
    }
    const float W = ui_w(), H = ui_h();
    draw_header(T(S_CHOOSE_COVER), g->g.title);
    const char *names[ROWS];
    const int n = lib_cover_candidates(g, C.q, names, ROWS);
    const bool was_editing = C.editing;
    In in = D.in;
    if (C.editing) {
        /* the keys go to the keyboard, typed or on the screen */
        if (D.in.text) {
            dash_text_type(C.q, sizeof(C.q), D.in.text);
        }
        if (D.in.key == SDLK_BACKSPACE || D.in.key == SDLK_DELETE || D.in.x) {
            dash_text_rub_out(C.q);
        }
        if (D.in.y) {
            dash_text_type(C.q, sizeof(C.q), " ");
        }
        if (D.in.back || D.in.key == SDLK_RETURN || D.in.key == SDLK_KP_ENTER) {
            edit(false);
            in.ok = false;
        }
        memset(&D.in, 0, sizeof(D.in));
        D.in.mx = D.in.my = -1;
        D.in.pad = in.pad;
    }
    const float ch = H - 300, cw = ch * 0.7f, px = 70, py = 150;
    const float lx = C.editing ? px : px + cw + 40, ly = py;
    const float lw = C.editing ? 560 : W - lx - 70, lh = H - ly - 110;
    List l;
    list_begin(&l, lx, ly, lw, lh, 52);
    l.focused = !C.editing && !D.ask;
    char shown[96];
    snprintf(shown, sizeof(shown), "%s%s", C.q[0] ? C.q : (C.editing ? "" : T(S_SEARCH_HINT)),
             C.editing && (int)(ui_time() * 2) % 2 ? "_" : "");
    if (list_row(&l, T(S_SEARCH), shown, NULL)) {
        edit(true);
    }
    const char *picked = NULL, *looked = NULL;
    for (int i = 0; i < n; i++) {
        char nm[300];
        row_name(names[i], nm, sizeof(nm));
        if (list_row(&l, nm, NULL, NULL)) {
            picked = names[i];
        }
        if (list_selected(&l)) {
            looked = names[i];
        }
    }
    const bool from_file = list_row(&l, T(S_COVER_FILE), NULL, NULL);
    list_end(&l);
    /* the list's state: being read, unreadable, no name fits */
    const char *note = n == -1 ? T(S_COVER_LOADING) : n == -2 ? T(S_COVER_OFFLINE) : n == 0 ? T(S_COVER_NONE) : NULL;
    if (note) {
        ui_text_fit(lx + 20, ly + 52 * 1.25f, note, 20, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT, lw - 40);
    }
    if (from_file) {
        edit(false);
        dash_pop();                     /* the game's screen takes the file (dash.c) */
        dash_cover_from_file();
        return;
    }
    if (picked) {
        if (lib_set_cover_name(g, picked)) {
            dash_toast(T(S_COVER_SET));
            edit(false);
            dash_pop();
            return;
        }
        bool failed = false;
        lib_cover_preview(picked, &failed);
        dash_toast(T(failed ? S_COVER_NOPIC : S_COVER_WAIT));
    }
    if (C.editing) {
        /* the keyboard beside the names */
        const float ks = 52, kx = lx + lw + 50;
        if (!was_editing) {
            /* started this frame (the search row chosen): the press isn't a key's */
            memset(&in, 0, sizeof(in));
            in.mx = in.my = -1;
            in.pad = D.in.pad;
        }
        if (dash_osk(kx, ly, ks, &in, C.q, sizeof(C.q))) {
            edit(false);
        }
        D.in = in;  /* the hints' glyphs: the input's own */
        if (in.pad) {
            draw_hints(T(S_KB_TYPE), T(S_KB_DONE), T(S_DELETE), T(S_KB_SPACE));
        } else {
            draw_hints(T(S_KB_DONE), NULL, T(S_DELETE), NULL);
        }
        return;
    }
    /* the selected name's picture, or the game's cover as it is */
    ui_round(px, py, cw, ch, 10, (UiColor){0x0E, 0x26, 0x07, 0xFF});
    bool failed = false;
    UiTex *t = looked ? lib_cover_preview(looked, &failed) : lib_cover(g);
    if (t) {
        fit_image(t, px + 6, py + 6, cw - 12, ch - 12);
    } else if (looked) {
        ui_text_fit(px + cw / 2, py + ch / 2 - 12, failed ? T(S_COVER_NOPIC) : "...", 22, UI_FONT_REGULAR, UI_GREY,
                    UI_ALIGN_CENTER, cw - 20);
    }
    ui_round_outline(px, py, cw, ch, 10, 1, UI_GREEN_DIM);
    if (D.in.back) {
        dash_pop();
    }
    draw_hints(T(S_SELECT), T(S_BACK), NULL, NULL);
}
