/*
 * xbased front end — controller mapping (input_map.h).
 */
#include "native/app/ui/input_map.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_name[IM_INPUTS] = {"a", "b", "x", "y", "black", "white", "lt", "rt", "start", "back",
                                              "ls", "rs", "up", "down", "left", "right", "lup", "ldown", "lleft",
                                              "lright", "rup", "rdown", "rleft", "rright", "press"};
static const char *const k_label[IM_INPUTS] = {
    "A", "B", "X", "Y", "Black", "White", "Left trigger", "Right trigger", "Start", "Back", "Left stick (click)",
    "Right stick (click)", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Left stick up", "Left stick down",
    "Left stick left", "Left stick right", "Right stick up", "Right stick down", "Right stick left",
    "Right stick right", "Pressure modifier"};
/* the keyboard's defaults (player 1) */
static const SDL_Keycode k_kb_def[IM_INPUTS] = {
    SDLK_SPACE, SDLK_LSHIFT, SDLK_Q, SDLK_E, SDLK_C, SDLK_Z, SDLK_1, SDLK_3, SDLK_RETURN, SDLK_BACKSPACE, SDLK_F, SDLK_H,
    SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_W, SDLK_S, SDLK_A, SDLK_D, SDLK_I, SDLK_K, SDLK_J, SDLK_L,
    SDLK_UNKNOWN};
/* the gamepad's: 1 + button, 1000 + axis * 2 + negative */
#define B(x) (1 + (x))
#define AX(a, neg) (1000 + (a) * 2 + (neg))
static const int k_pad_def[IM_INPUTS] = {
    B(SDL_GAMEPAD_BUTTON_SOUTH), B(SDL_GAMEPAD_BUTTON_EAST), B(SDL_GAMEPAD_BUTTON_WEST), B(SDL_GAMEPAD_BUTTON_NORTH),
    B(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), B(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), AX(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0),
    AX(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0), B(SDL_GAMEPAD_BUTTON_START), B(SDL_GAMEPAD_BUTTON_BACK),
    B(SDL_GAMEPAD_BUTTON_LEFT_STICK), B(SDL_GAMEPAD_BUTTON_RIGHT_STICK), B(SDL_GAMEPAD_BUTTON_DPAD_UP),
    B(SDL_GAMEPAD_BUTTON_DPAD_DOWN), B(SDL_GAMEPAD_BUTTON_DPAD_LEFT), B(SDL_GAMEPAD_BUTTON_DPAD_RIGHT),
    AX(SDL_GAMEPAD_AXIS_LEFTY, 1), AX(SDL_GAMEPAD_AXIS_LEFTY, 0), AX(SDL_GAMEPAD_AXIS_LEFTX, 1),
    AX(SDL_GAMEPAD_AXIS_LEFTX, 0), AX(SDL_GAMEPAD_AXIS_RIGHTY, 1), AX(SDL_GAMEPAD_AXIS_RIGHTY, 0),
    AX(SDL_GAMEPAD_AXIS_RIGHTX, 1), AX(SDL_GAMEPAD_AXIS_RIGHTX, 0), 0};
static const char *const k_hk_name[IM_HOTKEYS] = {"menu", "save", "load", "turbo", "turbotoggle", "pause", "shot", "full", "quit"};
static const SDL_Keycode k_hk_def[IM_HOTKEYS] = {SDLK_F1, SDLK_F5, SDLK_F8, SDLK_TAB, SDLK_F3, SDLK_F2, SDLK_F12, SDLK_F11, 0};
static const enum UiStr k_hk_label[IM_HOTKEYS] = {S_HK_MENU, S_HK_SAVE, S_HK_LOAD, S_HK_TURBO, S_HK_TURBO_TOGGLE,
                                                  S_HK_PAUSE, S_HK_SHOT, S_HK_FULL, S_HK_QUIT};

const char *im_input_label(int i)
{
    /* the face buttons and Start/Back as the controller prints them; the rest in the interface's language */
    static char buf[96];
    static const enum UiStr k_dir[4] = {S_DIR_UP, S_DIR_DOWN, S_DIR_LEFT, S_DIR_RIGHT};
    switch (i) {
    case IM_BLACK: return T(S_IN_BLACK);
    case IM_WHITE: return T(S_IN_WHITE);
    case IM_LT: return T(S_IN_LT);
    case IM_RT: return T(S_IN_RT);
    case IM_LS: return T(S_IN_LS);
    case IM_RS: return T(S_IN_RS);
    case IM_PRESS: return T(S_IN_PRESS);
    default: break;
    }
    if (i >= IM_DUP && i <= IM_DRIGHT) {
        snprintf(buf, sizeof(buf), "%s %s", T(S_IN_DPAD), T(k_dir[i - IM_DUP]));
        return buf;
    }
    if (i >= IM_LUP && i <= IM_LRIGHT) {
        snprintf(buf, sizeof(buf), "%s %s", T(S_IN_LSTICK), T(k_dir[i - IM_LUP]));
        return buf;
    }
    if (i >= IM_RUP && i <= IM_RRIGHT) {
        snprintf(buf, sizeof(buf), "%s %s", T(S_IN_RSTICK), T(k_dir[i - IM_RUP]));
        return buf;
    }
    return k_label[i];
}

enum UiStr im_hotkey_label(int h)
{
    return k_hk_label[h];
}

static const char *const k_mouse_name[6] = {NULL, "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1",
                                             "Mouse X2"};

static const char *key_name(SDL_Keycode k)
{
    if (k & IM_KEY_MOUSE) {
        const unsigned b = k & 7u;
        return b >= 1 && b <= 5 ? k_mouse_name[b] : "Mouse";
    }
    return SDL_GetKeyName(k);
}

static SDL_Keycode key_from_name(const char *n)
{
    for (unsigned b = 1; b <= 5; b++) {
        if (!SDL_strcasecmp(n, k_mouse_name[b])) {
            return IM_KEY_MOUSE | b;
        }
    }
    return SDL_GetKeyFromName(n);
}

static void map_key(int player, int input, char *out, size_t n)
{
    snprintf(out, n, "map.p%d.%s", player + 1, k_name[input]);
}

/* "<key name>|<pad element>" -> its parts, the defaults where absent */
static void parse(const Config *c, int player, int input, SDL_Keycode *key, int *pad)
{
    char k[48];
    map_key(player, input, k, sizeof(k));
    const char *v = cfg_get(c, k);
    *key = player == 0 ? k_kb_def[input] : SDLK_UNKNOWN;
    *pad = k_pad_def[input];
    if (!v) {
        return;
    }
    char kb[64];
    const char *bar = strchr(v, '|');
    snprintf(kb, sizeof(kb), "%.*s", bar ? (int)(bar - v) : (int)strlen(v), v);
    *key = kb[0] ? key_from_name(kb) : SDLK_UNKNOWN;
    const char *p = bar ? bar + 1 : "";
    if (p[0] == 'b') {
        *pad = 1 + atoi(p + 1);
    } else if (p[0] == 'a') {
        const int a = atoi(p + 1);
        *pad = 1000 + a * 2 + (strchr(p, '-') != NULL);
    } else {
        *pad = 0;
    }
}

static void store(Config *c, int player, int input, SDL_Keycode key, int pad)
{
    char k[48], v[96], pe[16] = "";
    map_key(player, input, k, sizeof(k));
    if (pad >= 1000) {
        snprintf(pe, sizeof(pe), "a%d%c", (pad - 1000) / 2, (pad & 1) ? '-' : '+');
    } else if (pad > 0) {
        snprintf(pe, sizeof(pe), "b%d", pad - 1);
    }
    snprintf(v, sizeof(v), "%s|%s", key ? key_name(key) : "", pe);
    cfg_set(c, k, v);
}

/* a pad's input as its face says it: A, LB, RT, Left stick X+, D-pad up */
static void pad_name(int pad, char *out, size_t n)
{
    static const char *const buttons[] = {"A", "B", "X", "Y", "Back", "Guide", "Start", "LS", "RS", "LB", "RB",
                                          "D-pad up", "D-pad down", "D-pad left", "D-pad right"};
    static const char *const axes[] = {"Left stick X", "Left stick Y", "Right stick X", "Right stick Y", "LT", "RT"};
    if (!pad) {
        snprintf(out, n, "-");
    } else if (pad >= 1000) {
        const int a = (pad - 1000) / 2;
        const char *nm = a < 6 ? axes[a] : SDL_GetGamepadStringForAxis((SDL_GamepadAxis)a);
        snprintf(out, n, "%s%s", nm ? nm : "axis", a >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "" : (pad & 1) ? "-" : "+");
    } else {
        const int b = pad - 1;
        const char *nm = b < 15 ? buttons[b] : SDL_GetGamepadStringForButton((SDL_GamepadButton)b);
        snprintf(out, n, "%s", nm ? nm : "?");
    }
}

void im_binding_name(const Config *c, int player, int input, char *out, size_t n)
{
    SDL_Keycode k;
    int p;
    parse(c, player, input, &k, &p);
    char pn[48];
    pad_name(p, pn, sizeof(pn));
    snprintf(out, n, "%s  /  %s", k ? key_name(k) : "-", pn);
}

void im_bind_key(Config *c, int player, int input, SDL_Keycode key)
{
    SDL_Keycode k;
    int p;
    parse(c, player, input, &k, &p);
    store(c, player, input, key, p);
}

void im_bind_button(Config *c, int player, int input, int button)
{
    SDL_Keycode k;
    int p;
    parse(c, player, input, &k, &p);
    store(c, player, input, k, 1 + button);
}

void im_bind_axis(Config *c, int player, int input, int sa)
{
    SDL_Keycode k;
    int p;
    parse(c, player, input, &k, &p);
    const int a = (sa < 0 ? -sa : sa) - 1;
    store(c, player, input, k, 1000 + a * 2 + (sa < 0));
}

void im_unbind(Config *c, int player, int input)
{
    store(c, player, input, SDLK_UNKNOWN, 0);
}

void im_clear_map(Config *c, int player)
{
    for (int i = 0; i < IM_INPUTS; i++) {
        char k[48];
        map_key(player, i, k, sizeof(k));
        cfg_unset(c, k);
    }
}

void im_copy_map(Config *to, const Config *from, int player)
{
    for (int i = 0; i < IM_INPUTS; i++) {
        SDL_Keycode k;
        int p;
        parse(from, player, i, &k, &p);
        store(to, player, i, k, p);
    }
}

static int device_of(const Config *c, int player)
{
    char k[16];
    snprintf(k, sizeof(k), "pad.p%d", player + 1);
    const char *v = cfg_get(c, k);
    if (!v || !strcmp(v, "auto")) {
        return 0;
    }
    if (!strcmp(v, "none")) {
        return -2;
    }
    if (!strcmp(v, "keyboard")) {
        return -1;
    }
    return !strncmp(v, "pad", 3) ? atoi(v + 3) : 0;
}

void im_device_name(const Config *c, int player, char *out, size_t n)
{
    const int d = device_of(c, player);
    if (d == -2) {
        snprintf(out, n, "%s", T(S_NONE));
    } else if (d == -1) {
        snprintf(out, n, "%s", T(S_KEYBOARD));
    } else if (d == 0) {
        snprintf(out, n, "%s", T(S_AUTO));
    } else {
        int cnt = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&cnt);
        const char *nm = ids && d <= cnt ? SDL_GetGamepadNameForID(ids[d - 1]) : NULL;
        snprintf(out, n, "%d: %s", d, nm ? nm : "-");
        SDL_free(ids);
    }
}

void im_cycle_device(Config *c, int player, int delta)
{
    int cnt = 0;
    SDL_free(SDL_GetGamepads(&cnt));
    /* auto, keyboard, pad1..padN, none */
    const int slots = cnt + 3;
    const int d = device_of(c, player);
    int pos = d == 0 ? 0 : d == -1 ? 1 : d == -2 ? slots - 1 : 1 + d;
    pos = (pos + delta + slots) % slots;
    char k[16], v[16];
    snprintf(k, sizeof(k), "pad.p%d", player + 1);
    if (pos == 0) {
        snprintf(v, sizeof(v), "auto");
    } else if (pos == 1) {
        snprintf(v, sizeof(v), "keyboard");
    } else if (pos == slots - 1) {
        snprintf(v, sizeof(v), "none");
    } else {
        snprintf(v, sizeof(v), "pad%d", pos - 1);
    }
    cfg_set(c, k, v);
}

void im_hotkey_name(const Config *c, int h, char *out, size_t n)
{
    char k[24];
    snprintf(k, sizeof(k), "hk.%s", k_hk_name[h]);
    const char *v = cfg_get(c, k);
    const char *name = v && *v ? v : SDL_GetKeyName(k_hk_def[h]);
    snprintf(out, n, "%s", *name ? name : "\xe2\x80\x94");  /* em dash when no key assigned */
}

void im_set_hotkey(Config *c, int h, SDL_Keycode key, SDL_Keymod mod)
{
    char k[24], v[96];
    snprintf(k, sizeof(k), "hk.%s", k_hk_name[h]);
    snprintf(v, sizeof(v), "%s%s%s%s", (mod & SDL_KMOD_CTRL) ? "Ctrl+" : "", (mod & SDL_KMOD_ALT) ? "Alt+" : "",
             (mod & SDL_KMOD_SHIFT) ? "Shift+" : "", SDL_GetKeyName(key));
    cfg_set(c, k, v);
}

/* "Ctrl+Alt+Shift+F5": the key, and the modifiers (1 Ctrl, 2 Alt, 4 Shift) */
static SDL_Keycode hotkey_parse(const char *v, uint8_t *mod)
{
    static const struct { const char *p; uint8_t bit; } k_mods[] = {{"Ctrl+", 1}, {"Alt+", 2}, {"Shift+", 4}};
    *mod = 0;
    for (bool more = true; more;) {
        more = false;
        for (unsigned i = 0; i < 3; i++) {
            const size_t n = strlen(k_mods[i].p);
            if (!SDL_strncasecmp(v, k_mods[i].p, n) && v[n]) {
                *mod |= k_mods[i].bit;
                v += n;
                more = true;
            }
        }
    }
    return SDL_GetKeyFromName(v);
}

/* the controller's choices for a shortcut */
static const struct { const char *v, *label; int code; } k_hkp[] = {
    {"none", NULL, IM_HKP_NONE},
    {"guide", "Guide", IM_HKP_GUIDE},
    {"touchpad", "Touchpad", IM_HKP_TOUCHPAD},
    {"share", "Share", IM_HKP_SHARE},
    {"guide+a", "Guide + A", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_SOUTH},
    {"guide+b", "Guide + B", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_EAST},
    {"guide+x", "Guide + X", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_WEST},
    {"guide+y", "Guide + Y", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_NORTH},
    {"guide+lb", "Guide + LB", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"guide+rb", "Guide + RB", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"guide+back", "Guide + Back", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_BACK},
    {"guide+start", "Guide + Start", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_START},
    {"guide+ls", "Guide + LS", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"guide+rs", "Guide + RS", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"guide+up", "Guide + D-pad up", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"guide+down", "Guide + D-pad down", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"guide+left", "Guide + D-pad left", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"guide+right", "Guide + D-pad right", IM_HKP_WITH + SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"back+a", "Back + A", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_SOUTH},
    {"back+b", "Back + B", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_EAST},
    {"back+x", "Back + X", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_WEST},
    {"back+y", "Back + Y", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_NORTH},
    {"back+lb", "Back + LB", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"back+rb", "Back + RB", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"back+start", "Back + Start", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_START},
    {"back+ls", "Back + LS", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"back+rs", "Back + RS", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"back+up", "Back + D-pad up", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"back+down", "Back + D-pad down", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"back+left", "Back + D-pad left", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"back+right", "Back + D-pad right", IM_HKP_BACK_WITH + SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};
/* EmuDeck-style defaults: Back (Select) as the modifier for every action.
 * Turbo uses a button (back+right) since triggers can't be BACK_WITH combos. */
static const char *const k_hkp_def[IM_HOTKEYS] = {
    "back+rs",    /* menu:        Select+R3 */
    "back+rb",    /* save:        Select+R1 */
    "back+lb",    /* load:        Select+L1 */
    "back+right", /* turbo:       Select+D-pad Right (R2 is a trigger; closest button) */
    "none",       /* turbotoggle: no default */
    "back+ls",    /* pause:       Select+L3 */
    "back+y",     /* shot:        Select+Y  */
    "none",       /* full:        no default pad combo */
    "back+start", /* quit:        Select+Start */
};

int im_hkpad_count(void)
{
    return (int)(sizeof(k_hkp) / sizeof(k_hkp[0]));
}

const char *im_hkpad_value(int i)
{
    return k_hkp[i].v;
}

const char *im_hkpad_label(int i)
{
    return k_hkp[i].label ? k_hkp[i].label : T(S_NONE);
}

const char *im_hkpad_default(int h)
{
    return k_hkp_def[h];
}

static int hkpad_code(const char *v)
{
    for (int i = 0; v && i < im_hkpad_count(); i++) {
        if (!SDL_strcasecmp(v, k_hkp[i].v)) {
            return k_hkp[i].code;
        }
    }
    return IM_HKP_NONE;
}

/* a stick's inverted axes (stick.linvert, stick.rinvert: off, y, x, xy),
 * the game's over the global one: 1 X, 2 Y */
static uint8_t invert_bits(const Config *game, const Config *global, const char *key)
{
    const char *v = game && cfg_get(game, key) ? cfg_get(game, key) : cfg_get(global, key);
    if (!v) {
        return 0;
    }
    return (strchr(v, 'x') ? 1 : 0) | (strchr(v, 'y') ? 2 : 0);
}

void im_resolve(ImMap *m, const Config *global, const Config *game)
{
    memset(m, 0, sizeof(*m));
    const bool custom = game && cfg_bool(game, "map.custom", false);
    for (int p = 0; p < 4; p++) {
        m->device[p] = device_of(global, p);
        for (int i = 0; i < IM_INPUTS; i++) {
            parse(custom ? game : global, p, i, &m->key[p][i], &m->pad[p][i]);
        }
    }
    for (int h = 0; h < IM_HOTKEYS; h++) {
        char k[24];
        snprintf(k, sizeof(k), "hk.%s", k_hk_name[h]);
        const char *v = cfg_get(global, k);
        m->hk_mod[h] = 0;
        m->hotkey[h] = v && *v ? hotkey_parse(v, &m->hk_mod[h]) : k_hk_def[h];
        snprintf(k, sizeof(k), "hkpad.%s", k_hk_name[h]);
        const char *pv = cfg_get(global, k);
        m->hk_pad[h] = hkpad_code(pv ? pv : k_hkp_def[h]);
    }
    const Config *dz = game && cfg_get(game, "deadzone") ? game : global;
    m->deadzone = cfg_int(dz, "deadzone", 10) / 100.0f;
    const Config *ad = game && cfg_get(game, "stick.antidz") ? game : global;
    m->stick_antidz = cfg_int(ad, "stick.antidz", 0) / 100.0f;
    const Config *sr = game && cfg_get(game, "stick.range") ? game : global;
    m->stick_range = cfg_int(sr, "stick.range", 100) / 100.0f;
    const Config *ss = game && cfg_get(game, "stick.shape") ? game : global;
    const char *shape = cfg_get(ss, "stick.shape");
    m->stick_round = shape && !strcmp(shape, "round");
    m->linvert = invert_bits(game, global, "stick.linvert");
    m->rinvert = invert_bits(game, global, "stick.rinvert");
    const Config *td = game && cfg_get(game, "trigger.deadzone") ? game : global;
    m->trigger_dz = cfg_int(td, "trigger.deadzone", 10) / 100.0f;
    const Config *tf = game && cfg_get(game, "trigger.full") ? game : global;
    m->trigger_full = cfg_int(tf, "trigger.full", 100) / 100.0f;
    const Config *pa = game && cfg_get(game, "press.amount") ? game : global;
    m->press_amount = cfg_int(pa, "press.amount", 50) / 100.0f;
    m->rumble = cfg_bool(global, "rumble", true);
    m->dc_pause = cfg_bool(global, "pad.dcpause", true);
    im_set_face(global);
    const Config *ms = game && cfg_get(game, "mouse.stick") ? game : global;
    const char *stick = cfg_get(ms, "mouse.stick");
    m->mouse_stick = !stick ? 0 : !strcmp(stick, "left") ? 1 : !strcmp(stick, "right") ? 2 : 0;
    const Config *mv = game && cfg_get(game, "mouse.sens") ? game : global;
    m->mouse_sens = cfg_int(mv, "mouse.sens", 100) / 100.0f;
}

static bool g_face_swap;

void im_set_face(const Config *global)
{
    const char *v = cfg_get(global, "pad.face");
    g_face_swap = v && !strcmp(v, "nintendo");
}

int im_face(int b)
{
    if (!g_face_swap) {
        return b;
    }
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return SDL_GAMEPAD_BUTTON_EAST;
    case SDL_GAMEPAD_BUTTON_EAST: return SDL_GAMEPAD_BUTTON_SOUTH;
    case SDL_GAMEPAD_BUTTON_WEST: return SDL_GAMEPAD_BUTTON_NORTH;
    case SDL_GAMEPAD_BUTTON_NORTH: return SDL_GAMEPAD_BUTTON_WEST;
    default: return b;
    }
}

static float pad_value(SDL_Gamepad *g, int pad)
{
    if (!g || !pad) {
        return 0;
    }
    if (pad < 1000) {
        return SDL_GetGamepadButton(g, (SDL_GamepadButton)im_face(pad - 1)) ? 1.0f : 0.0f;
    }
    const float v = SDL_GetGamepadAxis(g, (SDL_GamepadAxis)((pad - 1000) / 2)) / 32767.0f;
    return (pad & 1) ? (v < 0 ? -v : 0) : (v > 0 ? v : 0);
}

/* How hard a button of a controller that reports it is pressed, 0..1 (-1:
 * no such reading): the DualShock 3's face buttons, shoulders and d-pad are
 * pressure sensitive as the Duke's face buttons are, and SDL's PS3 drivers
 * give their pressure as ten axes after the six, in the order of the
 * buttons' enumeration (SDL_hidapi_ps3.c: sixteen axes, no other count).
 * Only while the button is down: a driver whose extra axes rest at their
 * centre would have every button half pressed. A clone that reports none (0
 * while pressed) is the button's press. */
static float pad_pressure(SDL_Gamepad *g, int button)
{
    static const signed char k_axis[SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1] = {
        [SDL_GAMEPAD_BUTTON_SOUTH] = 6, [SDL_GAMEPAD_BUTTON_EAST] = 7, [SDL_GAMEPAD_BUTTON_WEST] = 8,
        [SDL_GAMEPAD_BUTTON_NORTH] = 9, [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = 10,
        [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = 11, [SDL_GAMEPAD_BUTTON_DPAD_UP] = 12,
        [SDL_GAMEPAD_BUTTON_DPAD_DOWN] = 13, [SDL_GAMEPAD_BUTTON_DPAD_LEFT] = 14,
        [SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = 15};
    if (!g || button < 0 || button > SDL_GAMEPAD_BUTTON_DPAD_RIGHT || !k_axis[button] ||
        SDL_GetGamepadType(g) != SDL_GAMEPAD_TYPE_PS3) {
        return -1;
    }
    SDL_Joystick *j = SDL_GetGamepadJoystick(g);
    if (!j || SDL_GetNumJoystickAxes(j) != 16) {
        return -1;
    }
    if (!SDL_GetGamepadButton(g, (SDL_GamepadButton)button)) {
        return 0;
    }
    const float v = (SDL_GetJoystickAxis(j, k_axis[button]) + 32768) / 65535.0f;
    return v > 0.02f ? v : 1.0f;
}

/* An original Xbox pad (the Duke, the Controller S) on Linux's xpad driver
 * reports black and white as BTN_C and BTN_Z, which SDL's gamepad mapping
 * leaves out; SDL numbers the joystick's buttons in evdev code order (A 0,
 * B 1, C 2, X 3, Y 4, Z 5), so they are its raw buttons 2 and 5. Read as
 * well as the mapped ones, on those pads only (#286). */
static bool duke_bw(SDL_Gamepad *g, bool black)
{
#if defined(__linux__) && !defined(__ANDROID__)
    if (!g || SDL_GetGamepadVendor(g) != 0x045e) {
        return false;
    }
    const Uint16 pr = SDL_GetGamepadProduct(g);
    if (pr != 0x0202 && pr != 0x0285 && pr != 0x0287 && pr != 0x0289) {
        return false;
    }
    SDL_Joystick *j = SDL_GetGamepadJoystick(g);
    return j && SDL_GetNumJoystickButtons(j) >= 6 && SDL_GetJoystickButton(j, black ? 2 : 5);
#else
    (void)g;
    (void)black;
    return false;
#endif
}

/* A stick as the Duke gave it. Today's sticks move in a circle: pushed fully
 * diagonal they read about 0.7 on each axis, where the Duke's read nearly
 * full on both, and its games were tuned to that (Tony Hawk's Pro Skater
 * 2x's tricks, a direction and a button, took a diagonal for a straight
 * one, or none). So the dead zone is a circle around the centre, not a band
 * along each axis (that cut diagonals further), and the circle is stretched
 * onto the square: full deflection is full on both axes at any angle (round:
 * kept a circle). The range scales the result, for a stick that doesn't
 * reach its edge (above 100%) or games too quick to it (below). Keys
 * already give the square's corners and stay as they are. */
void im_stick(float x, float y, float dz, float antidz, float range, bool round, float *ox, float *oy)
{
    const float r = sqrtf(x * x + y * y);
    *ox = *oy = 0;
    if (r <= dz || r <= 0) {
        return;
    }
    float len = (r - dz) / (1 - dz);
    len = len > 1 ? 1 : len;
    /* the anti dead zone: past its own dead zone the stick starts at antidz,
     * not at 0, so a game's dead zone of that size is crossed at once (many
     * Xbox games ignore the first quarter or more of the travel themselves,
     * made for the Duke's loose sticks) */
    len = (antidz + len * (1 - antidz)) * range;
    const float ux = x / r, uy = y / r;
    const float m = round ? 1 : fabsf(ux) > fabsf(uy) ? fabsf(ux) : fabsf(uy);
    const float sx = ux / m * len, sy = uy / m * len;
    *ox = sx > 1 ? 1 : sx < -1 ? -1 : sx;
    *oy = sy > 1 ? 1 : sy < -1 ? -1 : sy;
}

/* A worn trigger that stops short of its end still reaches 255 (full), and
 * the travel past the dead zone is spread over the whole range, not cut off
 * (the old fixed 10% jumped from 0 to 26). Keys give 1, a full press. */
float im_trigger(float a, float dz, float full)
{
    if (a <= dz) {
        return 0;
    }
    if (full <= dz || a >= full) {
        return 1;
    }
    return (a - dz) / (full - dz);
}

static void stick(const ImMap *m, float xpos, float xneg, float ypos, float yneg, uint8_t inv, int16_t *ox,
                  int16_t *oy)
{
    float x, y;
    im_stick(xpos - xneg, ypos - yneg, m->deadzone, m->stick_antidz, m->stick_range, m->stick_round, &x, &y);
    *ox = (int16_t)((inv & 1 ? -x : x) * 32767);
    *oy = (int16_t)((inv & 2 ? -y : y) * 32767);
}

/* The mouse (im_mouse_move, im_mouse_buttons): the moves since the last
 * read, and the buttons down; the window's thread only. */
static struct {
    float dx, dy;                   /* moved since the last read */
    float sx, sy;                   /* the speed, smoothed (pixels a second) */
    uint32_t buttons;
    bool held;
    uint64_t last_ns;
} g_mouse;

void im_mouse_move(float dx, float dy)
{
    if (g_mouse.held) {
        g_mouse.dx += dx;
        g_mouse.dy += dy;
    }
}

void im_mouse_buttons(uint32_t mask, bool held)
{
    g_mouse.held = held;
    g_mouse.buttons = held ? mask : 0;
    if (!held) {
        g_mouse.dx = g_mouse.dy = g_mouse.sx = g_mouse.sy = 0;
    }
}

/* The mouse as a stick (#255). A stick tells a game where it is, a mouse
 * how far it moved: the moves since the last read over the time since are
 * its speed, smoothed over ~30 ms (reads come at the window's pace, the
 * moves in bursts), and the stick is pushed in proportion, full at 2000
 * pixels a second at 100% (a brisk turn), back at the centre as it stops.
 * The anti dead zone and the range then apply as to a stick: games turn
 * the stick with dead zones and curves of their own, which the anti dead
 * zone crosses. */
#define MOUSE_FULL_PPS 2000.0f
#define MOUSE_TAU_S 0.03f

static bool mouse_stick(const ImMap *m, float *ox, float *oy)
{
    const uint64_t now = SDL_GetTicksNS();
    float dt = g_mouse.last_ns ? (float)((now - g_mouse.last_ns) / 1e9) : 0.016f;
    g_mouse.last_ns = now;
    dt = dt < 0.001f ? 0.001f : dt > 0.1f ? 0.1f : dt;
    const float k = 1.0f - expf(-dt / MOUSE_TAU_S);
    g_mouse.sx += (g_mouse.dx / dt - g_mouse.sx) * k;
    g_mouse.sy += (g_mouse.dy / dt - g_mouse.sy) * k;
    g_mouse.dx = g_mouse.dy = 0;
    const float full = MOUSE_FULL_PPS / (m->mouse_sens > 0.05f ? m->mouse_sens : 0.05f);
    float x = g_mouse.sx / full, y = -g_mouse.sy / full;   /* the screen's down, the stick's up */
    const float r = sqrtf(x * x + y * y);
    if (r < 0.01f) {
        return false;
    }
    if (r > 1) {
        x /= r;
        y /= r;
    }
    im_stick(x, y, 0, m->stick_antidz, m->stick_range, m->stick_round, ox, oy);
    return true;
}

void im_read(const ImMap *m, int p, const bool *keys, SDL_Gamepad *pad, ImPad *out)
{
    im_read_mouse(m, p, keys, pad, false, out);
}

void im_read_mouse(const ImMap *m, int p, const bool *keys, SDL_Gamepad *pad, bool mouse, ImPad *out)
{
    float v[IM_INPUTS];
    for (int i = 0; i < IM_INPUTS; i++) {
        const SDL_Keycode k = m->key[p][i];
        float kv = 0;
        if (k & IM_KEY_MOUSE) {
            kv = mouse && (g_mouse.buttons & SDL_BUTTON_MASK(k & 7u)) ? 1.0f : 0.0f;
        } else if (keys && k) {
            const SDL_Scancode sc = SDL_GetScancodeFromKey(k, NULL);
            kv = keys[sc] ? 1.0f : 0.0f;
        }
        const int pe = m->pad[p][i];
        float pv = pad_value(pad, pe);
        if (i >= IM_A && i <= IM_WHITE && pe > 0 && pe < 1000) {
            /* the Duke's pressure-sensitive buttons: a controller's own pressure */
            const float pr = pad_pressure(pad, im_face(pe - 1));
            pv = pr >= 0 ? pr : pv;
        } else if (i >= IM_A && i <= IM_WHITE && (pe - 1000) / 2 >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
            /* ... or a trigger's travel, as the triggers take it */
            pv = im_trigger(pv, m->trigger_dz, m->trigger_full);
        }
        if ((i == IM_BLACK || i == IM_WHITE) && pv < 1 && duke_bw(pad, i == IM_BLACK)) {
            pv = 1;
        }
        v[i] = kv > pv ? kv : pv;
    }
    memset(out, 0, sizeof(*out));
    static const int k_bits[] = {IM_DUP, IM_DDOWN, IM_DLEFT, IM_DRIGHT, IM_START, IM_BACK, IM_LS, IM_RS};
    for (int b = 0; b < 8; b++) {
        if (v[k_bits[b]] > 0.5f) {
            out->buttons |= (uint16_t)(1u << b);
        }
    }
    for (int i = 0; i < 8; i++) {
        float a = v[IM_A + i];
        if (IM_A + i == IM_LT || IM_A + i == IM_RT) {
            a = im_trigger(a, m->trigger_dz, m->trigger_full);
            out->analog[i] = (uint8_t)(a >= 1 ? 255 : a * 255);
            continue;
        }
        out->analog[i] = (uint8_t)(a >= 1 ? 255 : a <= 0.1f ? 0 : a * 255);
        if (v[IM_PRESS] > 0.5f && out->analog[i]) {
            /* the pressure modifier held: a lighter press, never none */
            const float q = out->analog[i] * m->press_amount;
            out->analog[i] = (uint8_t)(q < 1 ? 1 : q);
        }
    }
    stick(m, v[IM_LRIGHT], v[IM_LLEFT], v[IM_LUP], v[IM_LDOWN], m->linvert, &out->lx, &out->ly);
    stick(m, v[IM_RRIGHT], v[IM_RLEFT], v[IM_RUP], v[IM_RDOWN], m->rinvert, &out->rx, &out->ry);
    float mx, my;
    if (mouse && m->mouse_stick && mouse_stick(m, &mx, &my)) {
        /* the mouse moving: its stick, the axes turned over as the stick's */
        const uint8_t inv = m->mouse_stick == 1 ? m->linvert : m->rinvert;
        int16_t *sx = m->mouse_stick == 1 ? &out->lx : &out->rx, *sy = m->mouse_stick == 1 ? &out->ly : &out->ry;
        *sx = (int16_t)((inv & 1 ? -mx : mx) * 32767);
        *sy = (int16_t)((inv & 2 ? -my : my) * 32767);
    }
}
