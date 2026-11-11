/*
 * xbased (hakuX for PC) — the front end: an SDL3 window, the controllers and
 * the keyboard, the core on its own thread, the HLE GPU's frames presented
 * into the window once per display refresh (present.c), as the Android
 * app's NativeCoreActivity and app_main.c do.
 *
 *   hakux [disc image] [option...]
 *
 * With no disc image, a file dialog asks for one. Options are the Android
 * app's arguments: the core's (native/core/options.h: scale=, loadstate=,
 * video=, lang=, net=, mods=, uncap, ...), the window's (aspect=auto|4:3|
 * 16:9|stretch, fxaa=0|1, upscale=bilinear|fsr), and
 *   hdd=DIR      the hard disk folder (default: hdd beside the executable)
 *   mod=ID       a mod of the game's on (its folder in mods\<TITLE ID>.<version>
 *                beside the executable; mod=* all of them), cheat=NAME a cheat
 *   fullscreen=0 start in a window (full screen, borderless, otherwise)
 *   console      a console window for the log (it always goes to xbased.log)
 * xbased.ini beside the executable holds options too, one a line ('#'
 * comments): the command line's come after, and win.
 *
 * Keys: F11 or Alt+Enter full screen, F2 pause, F5 save the state, F8 load
 * it (slot 1; Shift: slot 2), Esc leaves full screen. Player 1 plays on the
 * keyboard while no controller is plugged in (win_keys below).
 *
 * Loading a state, a title's relaunch (XLaunchNewImage) and its reboot
 * start the core again: the process runs itself again with the option the
 * Android app passes its activity (loadstate=, relaunch).
 */
#include "native/app/app.h"
#include "native/app/ui/config.h"
#include "native/app/ui/console_cfg.h"
#include "native/app/ui/dash.h"
#include "native/app/ui/i18n.h"
#include "native/app/ui/input_map.h"
#include "native/app/ui/ui.h"
#include "hw/xbox/hle/vk/swapchain.h"
#include "hw/xbox/hle/vk/share.h"
#include "native/app/ui/xbox_data.h"
#include "native/app/ui/xemu_import.h"
#include "native/app/win_aslr.h"
#include "native/core/fold_image.h"
#include "native/app/win_frame.h"
#ifndef _WIN32
#include <poll.h>
#include <sys/wait.h>
#endif
#include "native/app/win_auto.h"
#include "native/app/win_mods.h"
#include "native/core/cheats.h"
#include "native/core/core.h"
#include "native/core/mods.h"
#include "native/core/options.h"
#include "native/dev/usb.h"
#include "native/kernel/kernel.h"
#include "native/net/net.h"
#include "native/state/save.h"
#include "native/hle/hle_native.h"
#include "hw/xbox/hle/vk/native_window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "native/app/desk.h"   /* windows.h, or its few calls over POSIX */
#ifdef _WIN32
#include <shellapi.h>
#endif

/* native/dev/apu/out.c */
void nc_audio_set_paused(bool on);
void nc_audio_set_volume(float v);
/* native/dev/ohci.c */
void nc_dev_ohci_set_pads(unsigned mask);
/* hw/xbox/hle/pb/pbmode.h */
bool hle_pb_active(void);

#define MAX_ARGS 512

static NcCore g_core;
static int g_disc_fd = -1;
static SDL_AtomicInt g_core_state; /* 0 booting, 1 running, -1 failed or stopped */
static struct ANativeWindow g_nw;
static char g_exe_dir[MAX_PATH];
static char g_data_dir[MAX_PATH]; /* settings, saves, states, the log (cfg_data_dir) */
static char g_disc[1024];
static char *g_args[MAX_ARGS];     /* the options: xbased.ini's, the game's, the command line's */
static int g_nargs;
/* The command line's own options, which a run started again gets: the
 * settings' and the game's are read again by the new run. Passing all of
 * them on, each run added the settings again: a few relaunches or loaded
 * states filled the list, and what came last (the state to load, the
 * relaunch flag) was dropped, so a state loaded restarted the game, and a
 * title that relaunches (Chicago Enforcer) did so over and over. */
static char *g_cmdline[MAX_ARGS];
static int g_ncmdline;
static int g_ncfg_args;            /* how many of them xbased.ini gave */
static ImMap g_map;     /* the controls: the game's file over xbased.ini (input_map.h) */
static char g_key[24];  /* the game's "<TITLE ID>.<version>" */
static uint32_t g_title_id;  /* ... its title id */

/* ------------------------------------------------------------ helpers */

static void exe_dir(void)
{
    DWORD n = GetModuleFileNameA(NULL, g_exe_dir, sizeof(g_exe_dir));
    while (n && g_exe_dir[n - 1] != '\\' && g_exe_dir[n - 1] != '/') {
        n--;
    }
    g_exe_dir[n] = 0;
    snprintf(g_data_dir, sizeof(g_data_dir), "%s", cfg_data_dir());
}

static void add_arg(const char *a)
{
    static bool told;
    if (g_nargs < MAX_ARGS && a && a[0]) {
        g_args[g_nargs++] = strdup(a);
    } else if (a && a[0] && !told) {
        told = true;
        nc_log("hakuX-app", "more than %d options: %s and those after it left out", MAX_ARGS, a);
    }
}

/* xbased.ini: one option a line (hakux.ini, an earlier build's, becomes it) */
static void read_ini(void)
{
    char path[MAX_PATH + 16], old[MAX_PATH + 16];
    snprintf(path, sizeof(path), "%sxbased.ini", g_data_dir);
    snprintf(old, sizeof(old), "%shakux.ini", g_data_dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        MoveFileA(old, path);
    }
    {   /* the old shader settings gone first (cfg_shaders_migrate) */
        Config c;
        cfg_load(&c, path);
        if (cfg_shaders_migrate(&c)) {
            cfg_save(&c);
        }
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        size_t n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == '\t')) {
            p[--n] = 0;
        }
        if (n && p[0] != '#' && p[0] != ';') {
            add_arg(p);
        }
    }
    fclose(f);
}

static const char *opt_value(const char *name)
{
    const size_t n = strlen(name);
    const char *v = NULL;
    for (int i = 0; i < g_nargs; i++) {
        if (!strncmp(g_args[i], name, n) && g_args[i][n] == '=') {
            v = g_args[i] + n + 1; /* the last one wins */
        }
    }
    return v;
}

static bool opt_flag(const char *name)
{
    for (int i = 0; i < g_nargs; i++) {
        if (!strcmp(g_args[i], name)) {
            return true;
        }
    }
    return false;
}

/* Full screen (borderless, the desktop's mode) unless told otherwise
 * ("fullscreen=0"): a console's picture, not a window's. */
static bool want_fullscreen(void)
{
    const char *v = opt_value("fullscreen");
    return v ? strcmp(v, "0") != 0 : true;
}

/* The game's own options (games/<TITLE ID>.<version>.ini) after the global
 * ones and before the command line's (which come last and win); the
 * console's language and video flags as console_cfg.c keeps them; the dashboard's
 * "uncap=1" as the flag the core takes, "fullscreen" (on unless "=0") once. */
static void game_options(const char *disc)
{
    XdGame g;
    xd_read_game(disc, &g);
    xd_free_game(&g);
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%sgames/%s.ini", g_data_dir, g.key);
    Config c;
    cfg_load(&c, path);
    if (cfg_shaders_migrate(&c)) {
        cfg_save(&c);
    }
    /* the command line's options after the game's: move them to the end */
    char *cmdline[MAX_ARGS];
    int ncmd = 0;
    for (int i = g_ncfg_args; i < g_nargs; i++) {
        cmdline[ncmd++] = g_args[i];
    }
    g_nargs = g_ncfg_args;
    for (int i = 0; i < c.n; i++) {
        char a[600];
        if (c.flag[i]) {
            snprintf(a, sizeof(a), "%s", c.key[i]);
        } else {
            snprintf(a, sizeof(a), "%s=%s", c.key[i], c.val[i]);
        }
        add_arg(a);
    }
    for (int i = 0; i < ncmd; i++) {
        if (g_nargs < MAX_ARGS) {
            g_args[g_nargs++] = cmdline[i];
        }
    }
    if (c.n) {
        nc_log("hakuX-app", "%d options of the game's (%s)", c.n, path);
    }
    snprintf(g_key, sizeof(g_key), "%s", g.key);
    g_title_id = g.title_id;
    Config global;
    snprintf(path, sizeof(path), "%sxbased.ini", g_data_dir);
    cfg_load(&global, path);
    im_resolve(&g_map, &global, &c);
    /* flags written as values */
    static const char *const k_flags[] = {"uncap", "fullscreen"};
    for (int f = 0; f < 2; f++) {
        const char *v = opt_value(k_flags[f]);
        bool on = opt_flag(k_flags[f]) || f == 1;  /* full screen by default */
        if (v) {
            on = strcmp(v, "0") != 0;
        }
        for (int i = 0; i < g_nargs; i++) {
            const size_t n = strlen(k_flags[f]);
            if (!strncmp(g_args[i], k_flags[f], n) && (g_args[i][n] == '=' || g_args[i][n] == 0)) {
                g_args[i][0] = 0;  /* gone; added back once below */
            }
        }
        if (on) {
            add_arg(k_flags[f]);
        } else if (f == 1) {
            add_arg("fullscreen=0");
        }
    }
    i18n_set(opt_value("ui.lang"));     /* the app's messages */
    /* The console's language and video flags: what changed in the EEPROM
     * taken into the global settings first (console_cfg.c), then the
     * command line's, else the game's own, else the global ones (for "auto"
     * or none, the interface's language). The game's own language, or the
     * command line's, is kept even on a disc that seems to lack it. */
    cc_sync(&global);
    const char *cl_lang = NULL, *cl_video = NULL;
    for (int i = 0; i < ncmd; i++) {
        if (!strncmp(cmdline[i], "lang=", 5)) {
            cl_lang = cmdline[i] + 5;
        } else if (!strncmp(cmdline[i], "video=", 6)) {
            cl_video = cmdline[i] + 6;
        }
    }
    const int asked = cl_lang ? atoi(cl_lang) : cfg_get(&c, "lang") ? atoi(cfg_get(&c, "lang")) : 0;
    const bool fixed = asked >= 1 && asked <= 9;
    char a[32];
    snprintf(a, sizeof(a), "lang=%d", fixed ? asked : cc_lang(&global));
    add_arg(a);
    if (fixed) {
        add_arg("langfixed=1");
    }
    if (!cl_video && !cfg_get(&c, "video")) {
        snprintf(a, sizeof(a), "video=0x%x", cc_video(&global));
        add_arg(a);
    }
}

static void make_hdd(const char *root)
{
    char p[1024];
    mkdir(root, 0775);
    for (int i = 0; i <= 5; i++) {
        snprintf(p, sizeof(p), "%s/Partition%d", root, i);
        mkdir(p, 0775);
    }
}

/* The game's window has the screen: the one shown next (the dashboard back,
 * a relaunched title's) is to have it, with the keyboard and the gamepad.
 * Windows locks the foreground: without help that one stayed behind
 * whatever window came next, its taskbar button flashing, as if minimized.
 * Any process may take it (the grant); and back to the dashboard, the game
 * shows the dashboard's window and gives it the foreground itself (its
 * owner may give it to any window): the dashboard that started it says its
 * window in XBASED_DASH_HWND. The grant alone left the dashboard without
 * the gamepad on an ROG Xbox Ally X. While this process still has the
 * foreground: before its window hides (hidden, another window has it). */
static void give_foreground(bool to_dash)
{
#ifdef _WIN32
    AllowSetForegroundWindow(ASFW_ANY);
    const char *e = to_dash ? getenv("XBASED_DASH_HWND") : NULL;
    HWND h = e ? (HWND)(uintptr_t)strtoull(e, NULL, 16) : NULL;
    if (h && IsWindow(h)) {
        ShowWindow(h, SW_SHOW);
        SetForegroundWindow(h);
    }
#else
    (void)to_dash;
#endif
}

/* The process ends at once with code. On Windows ExitProcess would first
 * run every DLL's detach (the GPU driver's, the gamepads', the audio's) with
 * the emulation's threads cut off wherever they were, and one waiting on
 * another there hangs the process: a black full-screen window, and the
 * dashboard waiting behind it. TerminateProcess skips that; what the run
 * wrote is written (stderr isn't buffered, files are closed as they go).
 * A code with bit 30 is a run started again (run_again): no dashboard. */
static void *g_frame;   /* the session's frame (win_frame.h), NULL: the run's window on its own */
static bool g_frame_hidden;     /* the run's window out of sight in it: the frame presents the pictures */
static bool g_frame_keep;   /* this run ends for another (run_again): the frame stays */

static _Noreturn void exit_now(int code)
{
    const bool closing = g_frame && !(code & 0x40000000) && !g_frame_keep;
    if (closing) {
        nc_frame_close(g_frame);    /* the session's end: the frame hands the screen back to the dashboard */
    }
    fflush(NULL);
#ifdef _WIN32
    if (!closing) {
        give_foreground(!(code & 0x40000000));
    }
    TerminateProcess(GetCurrentProcess(), (UINT)code);
#endif
    _exit(code);
}

static void fatal_box(const char *title, const char *msg)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, NULL);
}

static void hold_picture(char *path, size_t n);
static void wait_next_run(void *ev, void *proc);

/* The process again, with the same disc and options plus extra (minus any
 * earlier option of extra's name). hold: the next run shows this one's
 * picture until its first frame (a state loaded, a title relaunched),
 * otherwise black (what this one shows at the end of a transition). On
 * Windows the next run's window comes up under this one, held on top, and
 * this one goes once that one has drawn: no desktop, nor a window without
 * the keyboard, between the two. */
static void run_again(const char *extra, bool hold)
{
    nc_kernel_set_paused(&g_core.kernel, true);    /* the picture held is the last one */
    nc_audio_set_paused(true);
    char hold_path[MAX_PATH + 32] = "";
    if (hold) {
        hold_picture(hold_path, sizeof(hold_path));
    }
    char cmd[8192];
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    int n = snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\"", exe, g_disc[0] ? g_disc : "none");
    const char *eq = extra ? strchr(extra, '=') : NULL;
    const size_t name_len = eq ? (size_t)(eq - extra) : extra ? strlen(extra) : 0;
    for (int i = 0; i < g_ncmdline && n < (int)sizeof(cmd) - 8; i++) {
        const char *a = g_cmdline[i];
        if (extra && !strncmp(a, extra, name_len) && (a[name_len] == '=' || a[name_len] == 0)) {
            continue;
        }
        if (!a[0] || !strcmp(a, "relaunch") || !strncmp(a, "loadstate=", 10)) {
            continue; /* one run's */
        }
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " \"%s\"", a);
    }
    if (extra) {
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " \"%s\"", extra);
    }
    nc_log("hakuX-app", "running again: %s", cmd);
#ifdef _WIN32
    SetEnvironmentVariableA("XBASED_HOLD", hold_path[0] ? hold_path : NULL);
    char ev_name[96];
    snprintf(ev_name, sizeof(ev_name), "Local\\xbased-ready-%lu-%llu", (unsigned long)GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, ev_name);
    SetEnvironmentVariableA("XBASED_READY", ev ? ev_name : NULL);
    char rect[64] = "";
    if (!g_frame && g_nw.sdl_window && !(SDL_GetWindowFlags(g_nw.sdl_window) & SDL_WINDOW_FULLSCREEN)) {
        int x = 0, y = 0, w = 0, h = 0;
        SDL_GetWindowPosition(g_nw.sdl_window, &x, &y);
        SDL_GetWindowSize(g_nw.sdl_window, &w, &h);
        snprintf(rect, sizeof(rect), "%d,%d,%d,%d", x, y, w, h);
    }
    SetEnvironmentVariableA("XBASED_RECT", rect[0] ? rect : NULL);
    if (g_nw.hwnd && !g_frame) {
        SetWindowPos((HWND)g_nw.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    PROCESS_INFORMATION pi;
    if (xb_spawn(exe, cmd, g_exe_dir, &pi)) {
        CloseHandle(pi.hThread);
        wait_next_run(ev, pi.hProcess);
        CloseHandle(pi.hProcess);
        /* the dashboard waits for this process: the new one's id in the
         * exit code (bit 30 set) lets it follow the game on (dash.c) */
        exit_now((int)(0x40000000u | (pi.dwProcessId & 0x3FFFFFFFu)));
    }
    if (g_nw.hwnd && !g_frame) {
        SetWindowPos((HWND)g_nw.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
#else
    if (hold_path[0]) {
        setenv("XBASED_HOLD", hold_path, 1);
    } else {
        unsetenv("XBASED_HOLD");
    }
    /* this process becomes the new run (the same arguments as above) */
    char *argv[MAX_ARGS + 4];
    int k = 0;
    argv[k++] = exe;
    argv[k++] = g_disc[0] ? g_disc : (char *)"none";
    for (int i = 0; i < g_ncmdline; i++) {
        char *a = g_cmdline[i];
        if (extra && !strncmp(a, extra, name_len) && (a[name_len] == '=' || a[name_len] == 0)) {
            continue;
        }
        if (!a[0] || !strcmp(a, "relaunch") || !strncmp(a, "loadstate=", 10)) {
            continue;
        }
        argv[k++] = a;
    }
    if (extra) {
        argv[k++] = (char *)extra;
    }
    argv[k] = NULL;
    fflush(stderr);
    if (g_frame) {
        /* inside the frame: the next run a process of its own, its window
         * coming up under this one's, this one going once it has drawn
         * (it writes to the pipe); exec'ing this one dropped its window */
        int rp[2];
        if (pipe(rp) == 0) {
            char fd[16];
            snprintf(fd, sizeof(fd), "%d", rp[1]);
            setenv("XBASED_READY_FD", fd, 1);
            const pid_t pid = fork();
            if (pid == 0) {
                close(rp[0]);
                execv(exe, argv);
                _exit(127);
            }
            close(rp[1]);
            unsetenv("XBASED_READY_FD");
            if (pid > 0) {
                wait_next_run((void *)(intptr_t)(rp[0] + 1), (void *)(intptr_t)pid);
                close(rp[0]);
                g_frame_keep = true;
                exit_now(0);
            }
            close(rp[0]);
        }
    }
    execv(exe, argv);
#endif
    exit_now(0);
}

/* ------------------------------------------------------------- the core */

static void *core_thread(void *arg)
{
    nc_win_thread_name("vcpu");
    nc_win_thread_priority(1);
    if (!nc_core_boot(&g_core, g_disc_fd)) {
        nc_log("hakuX-app", "core: boot failed");
        if (!nc_hle_fatal()) {
            nc_hle_set_fatal(NC_FATAL_BOOT, "The game stopped while starting.");
        }
        SDL_SetAtomicInt(&g_core_state, -1);
        return NULL;
    }
    SDL_SetAtomicInt(&g_core_state, 1);
    nc_core_run(&g_core);
    nc_log("hakuX-app", "core: stopped");
    SDL_SetAtomicInt(&g_core_state, -1);
    return NULL;
}

/* ------------------------------------------------------------ the input */

/* Controllers by player: each one plugged in takes the first free port. */
static SDL_Gamepad *g_pads[4];
static uint16_t g_rumble[4][2];


/* Player p's gamepad: its device's (auto: the p-th plugged in, padK: the
 * K-th), NULL for the keyboard, none, or nothing plugged. */
static SDL_Gamepad *player_pad(int p)
{
    const int d = g_map.device[p];
    if (d < 0) {
        return NULL;
    }
    const int k = d == 0 ? p : d - 1;
    return k < 4 ? g_pads[k] : NULL;
}

static void pads_changed(void)
{
    unsigned mask = 0;
    for (int p = 0; p < 4; p++) {
        const int d = g_map.device[p];
        /* player 1 always (the keyboard, unless set to none); the others with a pad */
        if (d != -2 && (player_pad(p) || (p == 0 && d <= 0) || d == -1)) {
            mask |= 1u << p;
        }
    }
    nc_dev_ohci_set_pads(mask ? mask : 1);
}

static void pad_added(SDL_JoystickID id)
{
    for (unsigned i = 0; i < 4; i++) {
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) {
            return;
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        if (!g_pads[i]) {
            g_pads[i] = SDL_OpenGamepad(id);
            nc_log("hakuX-app", "controller for player %u: %s", i + 1,
                   g_pads[i] ? SDL_GetGamepadName(g_pads[i]) : SDL_GetError());
            pads_changed();
            return;
        }
    }
}

/* A joystick SDL has no gamepad mapping for (the generic DirectInput pads):
 * the usual layout of those (buttons 0-3 the face buttons, 4-5 the
 * shoulders, 6-7 the triggers, 8-9 Back and Start, 10-11 the sticks'
 * clicks, the hat the d-pad, axes 0-1 and 2-3 the sticks), so it becomes a
 * gamepad. gamecontrollerdb.txt beside the executable, if there, has the
 * right one for many more. */
static void joystick_added(SDL_JoystickID id)
{
    if (SDL_IsGamepad(id)) {
        return;
    }
    char guid[64];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
    const char *name = SDL_GetJoystickNameForID(id);
    char map[512];
    snprintf(map, sizeof(map),
             "%s,%s,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,"
             "back:b8,start:b9,leftstick:b10,rightstick:b11,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
             "leftx:a0,lefty:a1,rightx:a2,righty:a3,",
             guid, name ? name : "Joystick");
    nc_log("hakuX-app", "joystick without a gamepad mapping: \"%s\" (%s); the generic layout given", name ? name : "?",
           guid);
    SDL_AddGamepadMapping(map); /* a gamepad now: SDL_EVENT_GAMEPAD_ADDED follows */
}

static void pad_removed(SDL_JoystickID id)
{
    for (unsigned i = 0; i < 4; i++) {
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) {
            SDL_CloseGamepad(g_pads[i]);
            g_pads[i] = NULL;
            nc_log("hakuX-app", "player %u's controller unplugged", i + 1);
            pads_changed();
        }
    }
}

/* Keys a shortcut with Ctrl or Alt took: the game doesn't see them until
 * they're let go (Ctrl+1 a shortcut, 1 the left trigger) */
static bool g_hk_eaten[SDL_SCANCODE_COUNT];
static bool g_hk_any_eaten;
/* Gamepads whose Guide button is held: the shortcuts' layer, the game
 * seeing that pad let go meanwhile (Guide + A no A in the game) */
static bool pad_guide_held(SDL_Gamepad *pad)
{
    return pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_GUIDE);
}

/* The pad whose Back + button shortcut went: let go for the game until
 * Back and that button are both up (Back + X no X in the game, even with
 * Back let go first) */
static SDL_Gamepad *g_back_pad;
static int g_back_btn;

static bool pad_in_shortcut(SDL_Gamepad *pad)
{
    if (!pad) {
        return false;
    }
    if (pad == g_back_pad) {
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) ||
            SDL_GetGamepadButton(pad, (SDL_GamepadButton)g_back_btn)) {
            return true;
        }
        g_back_pad = NULL;
    }
    return pad_guide_held(pad);
}

static void update_pads(bool focused)
{
    const bool *keys = SDL_GetKeyboardState(NULL);
    static bool kept[SDL_SCANCODE_COUNT];
    if (g_hk_any_eaten) {
        memcpy(kept, keys, sizeof(kept));
        bool any = false;
        for (int i = 0; i < SDL_SCANCODE_COUNT; i++) {
            if (g_hk_eaten[i] && !keys[i]) {
                g_hk_eaten[i] = false;      /* let go */
            }
            if (g_hk_eaten[i]) {
                kept[i] = false;
                any = true;
            }
        }
        g_hk_any_eaten = any;
        keys = kept;
    }
    for (unsigned i = 0; i < 4; i++) {
        NcPadState s;
        memset(&s, 0, sizeof(s));
        SDL_Gamepad *pad = player_pad((int)i);
        /* the keyboard: player 1 on auto, any player set to it */
        const bool kb = focused && (g_map.device[i] == -1 || (i == 0 && g_map.device[i] == 0));
        ImPad m;
        im_read(&g_map, (int)i, kb ? keys : NULL, pad_in_shortcut(pad) ? NULL : pad, &m);
        s.buttons = m.buttons;
        memcpy(s.analog, m.analog, sizeof(s.analog));
        s.lx = m.lx;
        s.ly = m.ly;
        s.rx = m.rx;
        s.ry = m.ry;
        if (i == 0) {
            nc_auto_pad(&s);
        }
        nc_pad_set(i, &s);
        uint16_t l = 0, r = 0;
        nc_pad_rumble(i, &l, &r);
        if (!g_map.rumble) {
            l = r = 0;
        }
        if (pad && (l != g_rumble[i][0] || r != g_rumble[i][1])) {
            SDL_RumbleGamepad(pad, l, r, l || r ? 1000 : 0);
            g_rumble[i][0] = l;
            g_rumble[i][1] = r;
        }
    }
}

/* ------------------------------------------------------------ states */

/* states\<TITLE ID>.<version>\slot<N>.hxs, its picture slot<N>.bmp beside */
static void state_path(int slot, char *out, size_t n)
{
    char dir[MAX_PATH + 64];
    snprintf(dir, sizeof(dir), "%sstates", g_data_dir);
    mkdir(dir, 0775);
    snprintf(dir, sizeof(dir), "%sstates/%s", g_data_dir, g_key);
    mkdir(dir, 0775);
    snprintf(out, n, "%s/slot%d.hxs", dir, slot);
}

/* The save runs on a thread of its own, as NativeCoreActivity's does: it
 * waits for the GPU to park at a frame, which the window's thread presents
 * meanwhile (from that thread, it waited on itself). */
static SDL_AtomicInt g_saving;
static SDL_AtomicInt g_saved_slot;  /* for the notice: slot + 1, or -1 not now (overlay_tick) */
static void notice(const char *text, const char *after_file);

static void *save_thread(void *arg)
{
    const int slot = (int)(intptr_t)arg;
    char path[MAX_PATH * 2];
    state_path(slot, path, sizeof(path));
    const char *err = nc_state_save(path);
    nc_log("hakuX-app", "savestate %d: %s", slot, err ? err : "saved");
    if (!err) {
        /* its picture: the frame shown, the dashboard's thumbnail */
        char pic[MAX_PATH * 2];
        snprintf(pic, sizeof(pic), "%.*s.bmp", (int)(strlen(path) - 4), path);
        remove(pic);
        hle_present_shot(pic);
        /* the notice after the picture: not in it */
        for (int i = 0; i < 100 && GetFileAttributesA(pic) == INVALID_FILE_ATTRIBUTES; i++) {
            SDL_Delay(10);
        }
    }
    SDL_SetAtomicInt(&g_saved_slot, err ? -1 : slot + 1);
    SDL_SetAtomicInt(&g_saving, 0);
    return NULL;
}

static void save_state(int slot)
{
    if (SDL_GetAtomicInt(&g_core_state) != 1 || !hle_pb_active()) {
        nc_log("hakuX-app", "savestate: not available now");
        SDL_SetAtomicInt(&g_saved_slot, -1);
        return;
    }
    if (!SDL_CompareAndSwapAtomicInt(&g_saving, 0, 1)) {
        return; /* one at a time */
    }
    pthread_t t;
    if (pthread_create(&t, NULL, save_thread, (void *)(intptr_t)slot) == 0) {
        pthread_detach(t);
    } else {
        SDL_SetAtomicInt(&g_saving, 0);
    }
}

static void load_state(int slot)
{
    char path[MAX_PATH * 2], opt[MAX_PATH * 2 + 16];
    state_path(slot, path, sizeof(path));
    const char *err = nc_state_compat(path);
    if (err) {
        nc_log("hakuX-app", "savestate %d: %s", slot, err);
        return;
    }
    snprintf(opt, sizeof(opt), "loadstate=%s", path);
    run_again(opt, true);
}

static bool g_uncap;    /* the run's frame rate setting (fast forward goes back to it) */
static int g_crop[2];   /* croph=, cropv= of the run's options */
static int g_maxfps = -1;   /* maxfps=: an uncapped game's limit (-1 the display's refresh, 0 none) */
static bool g_fs60;         /* fsrate=60: full screen in the panel's highest mode a multiple of 60 Hz */

/* The display's refresh rate (Hz, rounded; 60 if unknown) */
static int display_hz(SDL_Window *w)
{
    const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(w ? SDL_GetDisplayForWindow(w) : SDL_GetPrimaryDisplay());
    const int hz = m && m->refresh_rate > 0 ? (int)(m->refresh_rate + 0.5f) : 60;
    return hz < 60 ? 60 : hz;
}

/* Full screen, fsrate=60: the display's highest mode at its desktop size
 * whose refresh is a whole number of 60 Hz (59.94 too), no faster than the
 * desktop's: 120 Hz on a 144 or 165 Hz panel, 240 on a 280. The emulated
 * vblank then keeps to the panel's (nv2a.c), and every frame shows for as
 * long as on the console; at 144 Hz a 30 fps game's frames showed 28 and 35
 * ms in turn. Already a multiple, or none: the desktop's, borderless. */
static void fs_mode(SDL_Window *w)
{
    if (!g_fs60) {
        SDL_SetWindowFullscreenMode(w, NULL);
        return;
    }
    const SDL_DisplayID d = SDL_GetDisplayForWindow(w);
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(d);
    if (!dm || dm->refresh_rate <= 0) {
        SDL_SetWindowFullscreenMode(w, NULL);
        return;
    }
    /* the desktop's own rate: a run started again (a state loaded) comes
     * up while the last one's mode is still on the panel, and took that
     * for the desktop's (the game then stayed at the desktop's 280 Hz) */
    const char *was = getenv("XBASED_DESKTOP_HZ");
    const float r0 = was && atof(was) > 0 ? (float)atof(was) : dm->refresh_rate;
    if (!was) {
        char hz[24];
        snprintf(hz, sizeof(hz), "%.3f", r0);
        setenv("XBASED_DESKTOP_HZ", hz, 1);  /* for the runs this one starts */
    }
    const int k0 = (int)(r0 / 60.0f + 0.5f);
    if (k0 >= 1 && k0 <= 4 && fabsf(r0 - 60.0f * (float)k0) <= 0.01f * 60.0f * (float)k0) {
        SDL_SetWindowFullscreenMode(w, NULL);  /* the desktop's is one */
        return;
    }
    int n = 0;
    SDL_DisplayMode **ms = SDL_GetFullscreenDisplayModes(d, &n);
    const SDL_DisplayMode *best = NULL;
    for (int i = 0; ms && i < n; i++) {
        const SDL_DisplayMode *m = ms[i];
        const int k = (int)(m->refresh_rate / 60.0f + 0.5f);
        if (m->w == dm->w && m->h == dm->h && k >= 1 && k <= 4 && m->refresh_rate <= r0 + 0.5f &&
            fabsf(m->refresh_rate - 60.0f * (float)k) <= 0.01f * 60.0f * (float)k &&
            (!best || m->refresh_rate > best->refresh_rate)) {
            best = m;
        }
    }
    SDL_SetWindowFullscreenMode(w, best);
    nc_log("hakuX-app", "full screen: %s", best ? "the panel's highest mode a multiple of 60 Hz" : "the desktop's");
    if (best) {
        nc_log("hakuX-app", "full screen mode %dx%d at %.2f Hz (the desktop's %.2f)", best->w, best->h,
               best->refresh_rate, r0);
    }
    SDL_free(ms);
}
static int g_fps_cap;   /* ... its limit, uncapped (0: none) */
static float g_volume = 1.0f;
static bool g_mute_bg = true;   /* silent while the window isn't the active one */

/* screenshots\<title>-<date>.bmp beside the executable */
static void screenshot(void)
{
    char dir[MAX_PATH + 32], p[MAX_PATH + 128];
    snprintf(dir, sizeof(dir), "%sscreenshots", g_data_dir);
    mkdir(dir, 0775);
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(p, sizeof(p), "%s/%s-%04d%02d%02d-%02d%02d%02d.bmp", dir, g_key, t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond);
    hle_present_shot(p);
    notice(T(S_SHOT_SAVED), p);
}

/* ------------------------------------------------------------ the overlay */

/* The interface is the game's overlay (ui_init_overlay): its presenter
 * composites it over the picture in its own window pass, so the window is
 * never handed to anything else. Up only while something is drawn (the
 * in-game menu, a transition, a notice); otherwise the presenter draws as
 * if there were none. */
static bool g_fx = true;        /* ui.fx: the transitions */
static UiTex *g_hold;           /* the picture the run before this one left (XBASED_HOLD) */
static char g_hold_path[MAX_PATH + 32];
static int g_boot;              /* 0 before the first frame, 1 coming in, 2 done */
static double g_boot_t0;
static bool g_ov_up;            /* an overlay of the window loop's is set */
static char g_notice[200];
static double g_notice_t0;
static char g_notice_file[MAX_PATH + 160];  /* shown once this is written (a picture) */
#ifdef _WIN32
static HANDLE g_ready_ev;       /* the run before this one waits on it (run_again) */
#else
static int g_ready_fd = -1;     /* the run before this one waits on it (run_again, inside the frame) */
#endif

static void ov_present(void)
{
    if (g_nw.width <= 0 || !nc_present_frame(&g_nw)) {
        SDL_Delay(8);
    }
}

static void ov_size(int *w, int *h)
{
    uint32_t fw = 0, fh = 0;
    if (hle_present_remote()) {
        hle_present_remote_size(&fw, &fh);  /* the frame's pictures: its size */
    }
    *w = fw ? (int)fw : g_nw.width;
    *h = fh ? (int)fh : g_nw.height;
}

/* the frame's pointer (its pictures: the frame has the mouse) */
static float g_frame_mx = -1, g_frame_my = -1;

static bool frame_pointer(float *x, float *y)
{
    if (!hle_present_remote()) {
        return false;
    }
    *x = g_frame_mx;
    *y = g_frame_my;
    return true;
}

static bool ov_ok(void)
{
    return ui_is_overlay() && hle_ov_ready();
}

/* A picture of what the window shows now, for the next run (run_again). */
static void hold_picture(char *path, size_t n)
{
    path[0] = 0;
    if (!ov_ok()) {
        return;
    }
    ui_overlay_off();
    g_ov_up = false;
    snprintf(path, n, "%shandoff.bmp", g_data_dir);
    remove(path);
    hle_present_shot(path);
    for (int i = 0; i < 60 && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES; i++) {
        ov_present();
    }
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        path[0] = 0;
    }
}

/* The next run's window under this one's until it has drawn (its event),
 * it ended, or 15 seconds: this window keeps showing what it shows. */
static void wait_next_run(void *ev, void *proc)
{
#ifdef _WIN32
    const uint64_t until = SDL_GetTicks() + 15000;
    while (SDL_GetTicks() < until) {
        if ((ev && WaitForSingleObject((HANDLE)ev, 0) == WAIT_OBJECT_0) ||
            WaitForSingleObject((HANDLE)proc, 0) == WAIT_OBJECT_0) {
            break;
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
        }
        ov_present();
    }
    if (ev) {
        CloseHandle((HANDLE)ev);
    }
#else
    /* ev: the pipe's read end + 1, proc the next run's pid */
    const int fd = (int)(intptr_t)ev - 1;
    const pid_t pid = (pid_t)(intptr_t)proc;
    const uint64_t until = SDL_GetTicks() + 15000;
    while (fd >= 0 && SDL_GetTicks() < until) {
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, 0) > 0 || waitpid(pid, NULL, WNOHANG) == pid) {
            break;      /* drawn (a byte, or its end: gone) */
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
        }
        ov_present();
        SDL_Delay(4);
    }
#endif
}

/* Out of the game: its picture breaks into shards, black behind (what the
 * dashboard's window, or the next run's, comes up on). */
static void fx_out(void)
{
    if (!ov_ok()) {
        return;
    }
    const double t0 = ui_time();
    for (;;) {
        const float k = g_fx ? (float)((ui_time() - t0) / 0.5) : 1.0f;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
        }
        nc_auto_tick();     /* scripted pictures of it too */
        ui_begin();
        ui_cover(true);
        ui_fx_shatter(ui_scene(false), k);
        ui_end();
        if (k >= 1) {
            break;
        }
    }
    g_ov_up = true;     /* black stays up */
}

static void notice(const char *text, const char *after_file)
{
    snprintf(g_notice, sizeof(g_notice), "%s", text);
    snprintf(g_notice_file, sizeof(g_notice_file), "%s", after_file ? after_file : "");
    g_notice_t0 = after_file ? 0 : ui_time();
}

/* Each refresh of the window's loop, before it: the overlay it needs, if
 * any. Before the game's first frame, the picture the run before left (or
 * black); then that picture fading into the game's, or the game's rushing
 * in out of white; a notice over the game a moment. */
static void overlay_tick(void)
{
    if (!ui_is_overlay()) {
        return;
    }
    const int saved = SDL_SetAtomicInt(&g_saved_slot, 0);
    if (saved) {
        char t[200];
        if (saved > 0) {
            snprintf(t, sizeof(t), T(S_STATE_SAVED), saved - 1);
        } else {
            snprintf(t, sizeof(t), "%s", T(S_STATE_NOT_NOW));
        }
        notice(t, NULL);
    }
    if (g_notice_file[0] && GetFileAttributesA(g_notice_file) != INVALID_FILE_ATTRIBUTES) {
        g_notice_file[0] = 0;   /* written: the notice now, not in the picture */
        g_notice_t0 = ui_time();
    }
    const double now = ui_time();
    if (g_boot < 2) {
        const bool shown = nc_present_shown();
        if (!shown && (g_hold || g_fx)) {
            ui_begin();
            if (g_hold) {
                ui_fill(g_hold, UI_WHITE);
                /* a while on: darker, and the console's ring turning */
                const float d = (float)(now - 0.8);
                if (d > 0) {
                    ui_rect(0, 0, ui_w(), ui_h(), (UiColor){0, 0, 0, (uint8_t)(d > 1 ? 110 : 110 * d)});
                    const float a = d > 0.5f ? 1.0f : d * 2;
                    ui_ring(ui_w() - 64, ui_h() - 64, 22, 22, 0, 3, ui_alpha(UI_GREEN_DIM, 0.6f * a));
                    const float r = (float)(now * 5.0);
                    for (int i = 0; i < 3; i++) {
                        ui_circle(ui_w() - 64 + 22 * cosf(r + i * 0.35f), ui_h() - 64 + 22 * sinf(r + i * 0.35f),
                                  3.2f - i * 0.6f, ui_alpha(UI_GREEN_HI, a * (1 - i * 0.25f)));
                    }
                }
            }
            ui_overlay_submit(true);
            g_ov_up = true;
            return;
        }
        if (shown && g_boot == 0) {
            g_boot = 1;
            g_boot_t0 = now;
        }
        const double len = g_hold ? 0.3 : 0.42;
        if (g_boot == 1 && now - g_boot_t0 < len && (g_hold || g_fx)) {
            const float k = (float)((now - g_boot_t0) / len);
            ui_begin();
            if (g_hold) {
                ui_fill(ui_scene(false), UI_WHITE);
                ui_fill(g_hold, ui_alpha(UI_WHITE, 1 - k));
            } else {
                ui_fx_flash_in(ui_scene(false), k, (UiColor){255, 255, 255, 230});
            }
            ui_overlay_submit(true);
            g_ov_up = true;
            return;
        }
        if (shown || !(g_hold || g_fx)) {
            g_boot = 2;
            if (g_hold) {
                ui_texture_free(g_hold);
                g_hold = NULL;
                remove(g_hold_path);
            }
        }
    }
    if (g_notice[0] && g_notice_t0 > 0 && now - g_notice_t0 < 2.4) {
        const float t = (float)(now - g_notice_t0);
        const float a = t < 0.15f ? t / 0.15f : t > 2.0f ? (2.4f - t) / 0.4f : 1.0f;
        ui_begin();
        ui_set_alpha(a);
        const float sz = 22, w = ui_text_width(g_notice, sz, UI_FONT_REGULAR) + 56;
        const float x = (ui_w() - w) / 2, y = ui_h() - 96 + (1 - a) * 12;
        ui_round(x, y, w, 46, 23, (UiColor){0x08, 0x16, 0x05, 0xE0});
        ui_round_outline(x, y, w, 46, 23, 1.5f, ui_alpha(UI_GREEN, 0.7f));
        ui_glow(x + 24, y + 23, 14, ui_alpha(UI_GREEN, 0.8f));
        ui_text(x + 40, y + 10, g_notice, sz, UI_FONT_REGULAR, UI_WHITE, UI_ALIGN_LEFT);
        ui_overlay_submit(false);
        g_ov_up = true;
        return;
    }
    if (g_notice[0] && g_notice_t0 > 0) {
        g_notice[0] = 0;
    }
    if (g_ov_up) {
        ui_overlay_off();
        g_ov_up = false;
    }
}

/* What applies at once of the settings (the picture's shape and filters,
 * the volume): the game's over the global ones. */
static void apply_cfg(const Config *global, const Config *game, bool sound)
{
    const Config *pick[2] = {game, global};
    int crop_h = 0, crop_v = 0;     /* none unless set (a game's back to "global" leaves none) */
    for (int k = 1; k >= 0; k--) {
        const Config *c = pick[k];
        const char *v;
        if ((v = cfg_get(c, "aspect"))) {
            nc_present_set_aspect(!strcmp(v, "4:3") ? NC_ASPECT_4_3 : !strcmp(v, "16:9") ? NC_ASPECT_16_9
                                  : !strcmp(v, "stretch") ? NC_ASPECT_STRETCH : NC_ASPECT_AUTO);
        }
        if ((v = cfg_get(c, "overscan"))) {
            nc_present_set_overscan(atoi(v));
        }
        if ((v = cfg_get(c, "croph"))) {
            crop_h = atoi(v);
        }
        if ((v = cfg_get(c, "cropv"))) {
            crop_v = atoi(v);
        }
        if ((v = cfg_get(c, "fxaa"))) {
            nc_present_set_fxaa(atoi(v) != 0);
        }
        if ((v = cfg_get(c, "upscale"))) {
            nc_present_set_upscale(nc_upscale_from_name(v));
        }
        if ((v = cfg_get(c, "crt"))) {
            hle_present_set_crt(atoi(v));
        }
        if ((v = cfg_get(c, "vsync"))) {
            hle_present_set_vsync(atoi(v) != 0);
        }
        if ((v = cfg_get(c, "showfps"))) {
            nc_present_set_show_fps(atoi(v) != 0);
        }
        if (!sound) {
            continue;
        }
        if ((v = cfg_get(c, "volume"))) {
            g_volume = atoi(v) / 100.0f;
            nc_audio_set_volume(g_volume);
        }
        if ((v = cfg_get(c, "mutebg"))) {
            g_mute_bg = atoi(v) != 0;
        }
    }
    nc_present_set_crop(crop_h, crop_v);
    g_fx = cfg_bool(global, "ui.fx", true);
}

/* the menu's settings as they are being changed: the picture under it */
static void live_cfg(const struct Config *global, const struct Config *game)
{
    apply_cfg(global, game, false);
}

static void quit_to_dash(SDL_Window *win)
{
    /* straight out: nothing of the run is taken back first (the
     * presenter's swapchain, the paused threads), so nothing can hold the
     * window */
    nc_log("hakuX-app", "quit to the dashboard");
    if (!g_frame) {
        give_foreground(true);  /* else the frame hands it back (exit_now) */
    }
    SDL_HideWindow(win);
    exit_now(0);
}

/* The in-game menu: the emulation paused, the menu drawn by the game's own
 * presenter over its picture, blurred (the overlay). Without the overlay:
 * the presenter off the window (its Vulkan surface dropped) while the
 * dashboard's renderer draws the menu over the last frame; then the
 * presenter takes the window back. */
static void ingame_menu(SDL_Window *win, bool *paused, bool minimized)
{
    if (SDL_GetAtomicInt(&g_core_state) != 1) {
        return;
    }
    nc_kernel_set_paused(&g_core.kernel, true);
    nc_audio_set_paused(true);
    int slot = 0;
    const char *title = strrchr(g_disc, '\\');
    if (ov_ok()) {
        ui_overlay_off();
        g_ov_up = false;
        g_notice[0] = 0;
        const int act = dash_ingame(win, g_key, title ? title + 1 : g_disc, NULL, &slot, live_cfg);
        SDL_HideCursor();
        nc_auto_resync();
        if (act == DASH_INGAME_QUIT) {
            fx_out();
            quit_to_dash(win);
        }
        if (act == DASH_INGAME_RESTART) {
            fx_out();
            run_again(NULL, false);
        }
        ui_overlay_off();
        g_ov_up = false;
        nc_kernel_set_paused(&g_core.kernel, *paused || minimized);
        nc_audio_set_paused(*paused || minimized);
        char path[MAX_PATH + 64];
        snprintf(path, sizeof(path), "%sxbased.ini", g_data_dir);
        Config global, game;
        cfg_load(&global, path);
        snprintf(path, sizeof(path), "%sgames/%s.ini", g_data_dir, g_key);
        cfg_load(&game, path);
        im_resolve(&g_map, &global, &game);
        pads_changed();
        apply_cfg(&global, &game, true);
        if (act == DASH_INGAME_SAVE) {
            save_state(slot);
        } else if (act == DASH_INGAME_LOAD) {
            load_state(slot);
        }
        return;
    }
    char pic[MAX_PATH + 32];
    snprintf(pic, sizeof(pic), "%smenu_frame.bmp", g_data_dir);
    remove(pic);
    hle_present_shot(pic);
    for (int i = 0; i < 30 && GetFileAttributesA(pic) == INVALID_FILE_ATTRIBUTES; i++) {
        nc_present_frame(&g_nw);
        SDL_Delay(5);
    }
    nc_present_frame(NULL);     /* the surface goes */
    const int act = dash_ingame(win, g_key, title ? title + 1 : g_disc, pic, &slot, NULL);
    remove(pic);
    if (act == DASH_INGAME_QUIT) {
        quit_to_dash(win);
    }
    SDL_HideCursor();   /* the game doesn't use the mouse; the menu may have shown it */
    nc_auto_resync();
    /* the presenter back on the window before anything is shown or saved */
    SDL_Delay(50);
    nc_present_frame(&g_nw);
    nc_kernel_set_paused(&g_core.kernel, *paused || minimized);
    nc_audio_set_paused(*paused || minimized);
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%sxbased.ini", g_data_dir);
    Config global, game;
    cfg_load(&global, path);
    snprintf(path, sizeof(path), "%sgames/%s.ini", g_data_dir, g_key);
    cfg_load(&game, path);
    im_resolve(&g_map, &global, &game);
    pads_changed();
    apply_cfg(&global, &game, true);
    switch (act) {
    case DASH_INGAME_SAVE:
        save_state(slot);
        break;
    case DASH_INGAME_LOAD:
        load_state(slot);
        break;
    case DASH_INGAME_RESTART:
        run_again(NULL, false);
        break;
    default:
        break;
    }
}

/* debug.hakux.state ("save:<slot>", "load:<slot>"), as the app's menu would */
static void state_request_poll(void)
{
    static char last[PROP_VALUE_MAX];
    static bool primed;
    static unsigned n;
    if (n++ % 30) {
        return;
    }
    char v[PROP_VALUE_MAX] = "";
    __system_property_get("debug.hakux.state", v);
    if (!primed || !strcmp(v, last)) {
        primed = true;
        snprintf(last, sizeof(last), "%s", v);
        return;
    }
    snprintf(last, sizeof(last), "%s", v);
    if (!strncmp(v, "save:", 5)) {
        save_state(atoi(v + 5));
    } else if (!strncmp(v, "load:", 5)) {
        load_state(atoi(v + 5));
    }
}

/* ------------------------------------------------------------- main */

/* xbased.log beside the executable: a run the process started again (a
 * title's relaunch, a state loaded) goes on in the same log. */
static void log_setup(bool console, bool again)
{
    char path[MAX_PATH + 16];
    snprintf(path, sizeof(path), "%sxbased.log", g_data_dir);
#ifdef _WIN32
    if (console && AllocConsole()) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    } else {
        freopen(path, again ? "a" : "w", stderr);
    }
#else
    (void)console;
    freopen(path, again ? "a" : "w", stderr);
#endif
    setvbuf(stderr, NULL, _IONBF, 0);
}

/* The file dialog's answer (SDL calls back on a thread of its own). */
static SDL_AtomicInt g_dialog_done;
static char g_dialog_path[1024];

static void SDLCALL dialog_cb(void *ud, const char *const *list, int filter)
{
    if (list && list[0]) {
        snprintf(g_dialog_path, sizeof(g_dialog_path), "%s", list[0]);
    }
    SDL_SetAtomicInt(&g_dialog_done, 1);
}

static const char *pick_disc(void)
{
    static const SDL_DialogFileFilter filters[] = {
        {"Xbox disc images (*.iso, *.xiso)", "iso;xiso"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(dialog_cb, NULL, NULL, filters, 2, NULL, false);
    while (!SDL_GetAtomicInt(&g_dialog_done)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
        }
        SDL_Delay(20);
    }
    return g_dialog_path[0] ? g_dialog_path : NULL;
}

#ifndef XBASED_VERSION
#define XBASED_VERSION "1.0.104"
#endif

static void rm_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        remove(path);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        char p[1400];
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        struct stat st;
        if (!stat(p, &st) && S_ISDIR(st.st_mode)) {
            rm_tree(p);
        } else {
            remove(p);
        }
    }
    closedir(d);
    rmdir(path);
}

/* The shader caches (hle_spirv*, hle_pipeline_cache, hle_pipelines) from
 * another version go: each update starts them again, so nothing a build
 * before kept is used (they are keyed by their content, and old entries
 * only took space). The version they were made with: hle/version.txt. */
static void shader_cache_by_version(const char *base)
{
    if (!base) {
        return;
    }
    char vf[1200], seen[64] = "";
    snprintf(vf, sizeof(vf), "%s/version.txt", base);
    FILE *f = fopen(vf, "r");
    if (f) {
        if (fgets(seen, sizeof(seen), f)) {
            seen[strcspn(seen, "\r\n")] = 0;
        }
        fclose(f);
    }
    if (!strcmp(seen, XBASED_VERSION)) {
        return;
    }
    DIR *d = opendir(base);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strncmp(e->d_name, "hle_spirv", 9) || !strcmp(e->d_name, "hle_pipeline_cache") ||
                !strcmp(e->d_name, "hle_pipelines")) {
                char p[1300];
                snprintf(p, sizeof(p), "%s/%s", base, e->d_name);
                rm_tree(p);
            }
        }
        closedir(d);
    }
    f = fopen(vf, "w");
    if (f) {
        fprintf(f, "%s\n", XBASED_VERSION);
        fclose(f);
    }
    nc_log("hakuX-app", "shader caches of %s cleared (%s)", seen[0] ? seen : "an earlier version",
           XBASED_VERSION);
}
#ifdef _WIN32
#define NC_PLATFORM_NAME "Windows"
#else
#define NC_PLATFORM_NAME "Linux"
#endif

/* A shortcut pressed (down) or let go: only fast forward lasts while held;
 * slot: quick save and load's (Shift with the key: 2) */
static int g_pad_held_hk = -1, g_pad_held_code;
static bool g_guide_alone;      /* Guide pressed, nothing with it yet: a tap opens the menu */

static void hotkey_do(SDL_Window *win, int h, bool down, int slot, bool *paused, bool minimized)
{
    if (!down) {
        if (h == IM_HK_TURBO) {
            nc_win_prop_set("debug.hakux.uncap", g_uncap ? "1" : "0");
            char cap[16];
            snprintf(cap, sizeof(cap), "%d", g_fps_cap);
            nc_win_prop_set("debug.hakux.fpscap", cap);
        }
        return;
    }
    switch (h) {
    case IM_HK_FULL:
        if (g_frame) {
            nc_frame_toggle_fullscreen(g_frame);
            break;
        }
        fs_mode(win);
        SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
        break;
    case IM_HK_PAUSE:
        *paused = !*paused;
        nc_kernel_set_paused(&g_core.kernel, *paused || minimized);
        nc_audio_set_paused(*paused || minimized);
        nc_log("hakuX-app", "%s", *paused ? "paused" : "resumed");
        break;
    case IM_HK_SAVE:
        save_state(slot);
        break;
    case IM_HK_LOAD:
        load_state(slot);
        break;
    case IM_HK_TURBO:
        nc_win_prop_set("debug.hakux.uncap", "1");  /* frames as fast as they come, while held */
        nc_win_prop_set("debug.hakux.fpscap", "0");
        break;
    case IM_HK_SHOT:
        screenshot();
        break;
    case IM_HK_MENU:
        ingame_menu(win, paused, minimized);
        break;
    case IM_HK_QUIT:
        nc_log("xbased", "quit to the dashboard");
        _exit(0);
    default:
        break;
    }
}

/* The in-game menu's full screen key (dash.c): the frame's */
static void frame_fullscreen(void)
{
    nc_frame_toggle_fullscreen(g_frame);
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
    if (argc > 1 && !strcmp(argv[1], "frame")) {
        return nc_frame_main(argc, argv);      /* the game's frame (win_frame.h) */
    }
    xb_aslr_check();    /* Windows' first memory where the guest's goes: run again without (win_aslr.h) */
#endif
    /* The gamepads with the window in the background (the dashboard behind a
     * launcher, a game after a click elsewhere): as the environment's, which
     * SDL takes where the hint set at run time didn't (a virtual gamepad's
     * moves behind another window were dropped with it), inherited by the
     * game's process. */
    if (!getenv("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS")) {
#ifdef _WIN32
        _putenv("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1");
#else
        setenv("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 0);
#endif
        SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", false);
    }
    exe_dir();
    read_ini();
    g_ncfg_args = g_nargs;
    const char *disc = NULL;
    for (int i = 1; i < argc; i++) {
        if (!disc && !strchr(argv[i], '=') && strcmp(argv[i], "uncap") && strcmp(argv[i], "relaunch") &&
            strcmp(argv[i], "fullscreen") && strcmp(argv[i], "console") && strcmp(argv[i], "dash")) {
            disc = argv[i];
        } else {
            add_arg(argv[i]);
            if (g_ncmdline < MAX_ARGS && argv[i][0]) {
                g_cmdline[g_ncmdline++] = strdup(argv[i]);
            }
        }
    }
    bool again = opt_flag("relaunch");
    for (int i = 0; i < g_nargs; i++) {
        again |= !strncmp(g_args[i], "loadstate=", 10);
    }
#ifndef _WIN32
    {
        /* a game's run, X11: this process stays as the session's frame, a
         * child of it goes on as the run (win_frame.h); frame=0, the
         * dashboard or a disc to pick: as before */
        const char *fv = opt_value("frame");
        const char *fr = opt_value("fsrate");
        if (disc && strcmp(disc, "none") && !opt_flag("pick") && (!fv || strcmp(fv, "0"))) {
            nc_frame_fork(want_fullscreen(), fr && !strcmp(fr, "60"));
        }
    }
#endif
    log_setup(opt_flag("console"), again);
    if (again) {
        nc_log("hakuX-app", "---- started again (%s)", opt_flag("relaunch") ? "the title's relaunch" : "a state loaded");
    }
    nc_log("hakuX-app", "xbased " XBASED_VERSION " (an original Xbox emulator by crazyriddler, based on hakuX), " NC_PLATFORM_NAME);
    nc_win_mods_cleanup(g_exe_dir);     /* what an update took away (win_mods.h) */

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifndef _WIN32
    /* PulseAudio's API first: PipeWire desktops serve it too, and where
     * PipeWire's own is missing (WSLg) SDL waits half a minute on it */
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "pulseaudio,pipewire,alsa");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fatal_box("xbased", SDL_GetError());
        return 1;
    }
    {
        char db[MAX_PATH + 32];
        snprintf(db, sizeof(db), "%sgamecontrollerdb.txt", g_exe_dir);
        const int n = SDL_AddGamepadMappingsFromFile(db);
        if (n > 0) {
            nc_log("hakuX-app", "%d gamepad mappings from %s", n, db);
        }
    }
    /* "dash" with no disc: the Xbox dashboard from the hard disk's C: drive,
     * the tray empty (its Xbox Live account pages, its settings) */
    const bool xbox_dash = opt_flag("dash") && (!disc || !strcmp(disc, "none"));
    if (!xbox_dash && (!disc || !strcmp(disc, "none"))) {
        if (opt_flag("pick")) {
            disc = pick_disc();
            if (!disc) {
                return 0;
            }
        } else {
            /* the dashboard: the library and settings; each game in a process of its own */
            SDL_Window *dw = SDL_CreateWindow("xbased", 1280, 720,
                                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                                  (want_fullscreen() ? SDL_WINDOW_FULLSCREEN : 0));
            if (!dw) {
                fatal_box("xbased", SDL_GetError());
                return 1;
            }
            char exe[MAX_PATH];
            GetModuleFileNameA(NULL, exe, sizeof(exe));
            const int r = dash_run(dw, exe);
            SDL_DestroyWindow(dw);
            SDL_Quit();
            return r;
        }
    }
    if (!xbox_dash) {
        snprintf(g_disc, sizeof(g_disc), "%s", disc);
        game_options(g_disc);
        /* an extracted game: its folder (or default.xbe) read as the disc it makes */
        char xbe[MAX_PATH * 2];
        g_disc_fd = open(nc_fold_xbe_path(g_disc, xbe, sizeof(xbe)) ? xbe : g_disc, O_RDONLY | O_BINARY);
        if (g_disc_fd < 0) {
            nc_log("hakuX-app", "can't open the disc %s", g_disc);
        }
    } else {
        /* the controls as a game's (game_options), xbased.ini's alone: left
         * unresolved, the Xbox dashboard got no controller until the in-game
         * menu resolved them on Resume (#124) */
        char path[MAX_PATH + 16];
        snprintf(path, sizeof(path), "%sxbased.ini", g_data_dir);
        Config global, none;
        cfg_load(&global, path);
        memset(&none, 0, sizeof(none));
        im_resolve(&g_map, &global, &none);
    }
    char hdd[1024];
    const char *hv = opt_value("hdd");
    if (hv) {
        snprintf(hdd, sizeof(hdd), "%s", hv);
    } else {
        snprintf(hdd, sizeof(hdd), "%shdd", g_data_dir);
    }
    make_hdd(hdd);
    if (g_title_id) {
        /* downloaded content and title updates copied in by hand: signed for this console */
        const int resigned = xi_resign_downloads(hdd, g_title_id);
        if (resigned) {
            nc_log("hakuX-app", "%d downloaded content file(s) of %08x re-signed for this console", resigned, g_title_id);
        }
    }
    nc_io_set_hdd_root(hdd);
    setenv("HAKUX_HLE_MODE", "replace", 0);
    {
        char base[1100];
        snprintf(base, sizeof(base), "%s/../hle", hdd);
        mkdir(base, 0775);
        setenv("HAKUX_NATIVE_BASE", base, 0);
        shader_cache_by_version(getenv("HAKUX_NATIVE_BASE"));
    }

    NcOptions opt;
    nc_options_init(&opt);
    for (int i = 0; i < g_nargs; i++) {
        const char *a = g_args[i];
        char resolved[2400];
        if (!strncmp(a, "xcheat=", 7) && a[7] && a[7] != '/' && a[7] != '\\' && a[8] != ':') {
            /* a cheat file under cheats/ (the shipped ones beside the
             * executable, the user's in the data folder) */
            char p[1300];
            snprintf(p, sizeof(p), "%s", a + 7);
            char *bar = strrchr(p, '|');
            const char *states = bar ? bar + 1 : "";
            if (bar) {
                *bar = 0;
            }
            char full[1400];
            snprintf(full, sizeof(full), "%s%s", g_data_dir, p);
            if (access(full, 0) != 0) {
                snprintf(full, sizeof(full), "%s%s", g_exe_dir, p);
            }
            snprintf(resolved, sizeof(resolved), "xcheat=%s|%s", full, states);
            a = resolved;
        }
        if (nc_option(&opt, a)) {
            continue; /* the core's */
        }
        if (!strncmp(a, "aspect=", 7)) {
            const char *v = a + 7;
            nc_present_set_aspect(!strcmp(v, "4:3")       ? NC_ASPECT_4_3
                                  : !strcmp(v, "16:9")    ? NC_ASPECT_16_9
                                  : !strcmp(v, "stretch") ? NC_ASPECT_STRETCH
                                                          : NC_ASPECT_AUTO);
        } else if (!strncmp(a, "overscan=", 9)) {
            nc_present_set_overscan(atoi(a + 9));
        } else if (!strncmp(a, "maxfps=", 7)) {
            g_maxfps = !strcmp(a + 7, "refresh") ? -1 : atoi(a + 7);
        } else if (!strncmp(a, "fsrate=", 7)) {
            g_fs60 = !strcmp(a + 7, "60");
        } else if (!strncmp(a, "croph=", 6)) {
            g_crop[0] = atoi(a + 6);
            nc_present_set_crop(g_crop[0], g_crop[1]);
        } else if (!strncmp(a, "cropv=", 6)) {
            g_crop[1] = atoi(a + 6);
            nc_present_set_crop(g_crop[0], g_crop[1]);
        } else if (!strncmp(a, "fxaa=", 5)) {
            nc_present_set_fxaa(atoi(a + 5) != 0);
        } else if (!strncmp(a, "upscale=", 8)) {
            nc_present_set_upscale(nc_upscale_from_name(a + 8));
        } else if (!strncmp(a, "crt=", 4)) {
            hle_present_set_crt(atoi(a + 4));
        } else if (!strncmp(a, "vsync=", 6)) {
            hle_present_set_vsync(atoi(a + 6) != 0);
        } else if (!strncmp(a, "volume=", 7)) {
            g_volume = atoi(a + 7) / 100.0f;
            nc_audio_set_volume(g_volume);
        } else if (!strncmp(a, "showfps=", 8)) {
            nc_present_set_show_fps(atoi(a + 8) != 0);
        } else if (!strncmp(a, "mutebg=", 7)) {
            g_mute_bg = atoi(a + 7) != 0;
        }
    }
    nc_win_mods_prepare(g_exe_dir, g_disc_fd, g_args, g_nargs);
    if (opt.uncap && opt.fps_cap <= 0 && g_maxfps != 0) {
        /* uncapped with no limit of the game's own: the global one, never
         * under 60 (games without a frame cap of their own ran at hundreds) */
        opt.fps_cap = g_maxfps < 0 ? display_hz(NULL) : g_maxfps < 60 ? 60 : g_maxfps;
    }
    nc_options_apply(&opt);
    g_uncap = opt.uncap;
    g_fps_cap = opt.fps_cap;
    nc_present_set_widescreen(opt.video >= 0 && (opt.video & NC_VIDEO_WIDESCREEN));
    nc_log("hakuX-app", "native core: disc %s, hdd %s%s", g_disc, hdd, opt.uncap ? ", frame rate uncapped" : "");

    const char *title = strrchr(g_disc, '\\');
    const char *title2 = strrchr(g_disc, '/');
    title = title2 > title ? title2 : title;
    char wtitle[512];
    snprintf(wtitle, sizeof(wtitle), "xbased - %s", xbox_dash ? "Xbox Dashboard" : title ? title + 1 : g_disc);
#ifdef _WIN32
    const SDL_WindowFlags vk_flag = 0;            /* the presenter makes its own Win32 surface */
#else
    const SDL_WindowFlags vk_flag = SDL_WINDOW_VULKAN; /* SDL_Vulkan_CreateSurface's (swapchain.c) */
#endif
    const SDL_WindowFlags hidden = SDL_WINDOW_HIDDEN;   /* shown inside the frame, or on its own below */
    SDL_Window *win = SDL_CreateWindow(wtitle, 1280, 960,
                                       vk_flag | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | hidden);
    if (!win) {
        fatal_box("xbased", SDL_GetError());
        return 1;
    }
    {
        /* the session's frame (win_frame.h): a run started again comes up
         * inside the same window; frame=0 the run's window on its own */
        const char *fv = opt_value("frame");
        if (!fv || strcmp(fv, "0")) {
            g_frame = nc_frame_get(wtitle, want_fullscreen(), g_fs60);
        }
        if (g_frame) {
            /* the frame presents this run's pictures (hle_present_set_remote):
             * its window out of sight; else (no token, or they can't be had,
             * below) filling the frame and presenting itself */
            const char *tok = nc_frame_token();
            g_frame_hidden = tok && tok[0];
            if (g_frame_hidden) {
                hle_present_set_remote(tok);
                dash_set_pointer_hook(frame_pointer);
            }
            nc_frame_adopt(g_frame, win, wtitle, g_frame_hidden);
            dash_set_fullscreen_hook(frame_fullscreen);
            nc_log("hakuX-app", "window: inside the session's frame");
        }
    }
    if (!g_frame) {
        fs_mode(win);       /* the full screen's mode before it's full screen */
        if (want_fullscreen()) {
            SDL_SetWindowFullscreen(win, true);
        }
        /* the run before's window's place (run_again), windowed */
        const char *r = getenv("XBASED_RECT");
        int x, y, w, h;
        if (r && sscanf(r, "%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w > 0 && h > 0 &&
            !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)) {
            SDL_SetWindowSize(win, w, h);
            SDL_SetWindowPosition(win, x, y);
        }
        SDL_ShowWindow(win);
    }
    SDL_HideCursor();   /* the game doesn't use the mouse */
    SDL_PropertiesID props = SDL_GetWindowProperties(win);
    g_nw.hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    g_nw.hinstance = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, NULL);
    g_nw.sdl_window = win;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    g_nw.width = pw;
    g_nw.height = ph;

    if (!nc_present_init()) {
        return 1;
    }
    {
        /* the interface as the game's overlay: the in-game menu, the
         * transitions, the notices (its fonts beside the executable) */
        char fonts[MAX_PATH + 16];
        snprintf(fonts, sizeof(fonts), "%sfonts/", g_exe_dir);
        if (!ui_init_overlay(fonts, ov_present, ov_size)) {
            nc_log("hakuX-app", "no overlay: the in-game menu on a renderer of its own");
        }
        char path[MAX_PATH + 16];
        snprintf(path, sizeof(path), "%sxbased.ini", g_data_dir);
        Config global;
        cfg_load(&global, path);
        g_fx = cfg_bool(&global, "ui.fx", true);
        const char *hp = getenv("XBASED_HOLD");
        if (hp && hp[0] && ui_is_overlay()) {
            SDL_Surface *sf = SDL_LoadBMP(hp);
            if (sf) {
                g_hold = ui_texture_surface(sf);
                SDL_DestroySurface(sf);
                snprintf(g_hold_path, sizeof(g_hold_path), "%s", hp);
            }
        }
#ifdef _WIN32
        const char *rv = getenv("XBASED_READY");
        if (rv && rv[0]) {
            g_ready_ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, rv);
        }
        SetEnvironmentVariableA("XBASED_READY", NULL);
        SetEnvironmentVariableA("XBASED_HOLD", NULL);
        SetEnvironmentVariableA("XBASED_RECT", NULL);
#else
        unsetenv("XBASED_HOLD");
        const char *rf = getenv("XBASED_READY_FD");
        if (rf && rf[0]) {
            g_ready_fd = atoi(rf);
        }
        unsetenv("XBASED_READY_FD");
#endif
    }
    nc_vsync_start();
    nc_auto_init(g_data_dir, save_state, load_state);
#if defined(__x86_64__) || (defined(__aarch64__) && defined(__linux__))
    nc_core_fault_install(&g_core);
#endif
    pads_changed();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_t core;
    pthread_create(&core, &attr, core_thread, NULL);
    pthread_attr_destroy(&attr);

    nc_win_thread_name("window");
    bool quit = false, paused = false, minimized = false, focused = true, told = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN: {
                if (ev.key.repeat) {
                    break;
                }
                /* the shortcuts (Settings > Shortcuts): the key with the
                 * Ctrl and Alt it was set with (exactly), Shift too if it
                 * was; Shift with quick save/load otherwise: slot 2 */
                const uint8_t held = ((ev.key.mod & SDL_KMOD_CTRL) ? 1 : 0) | ((ev.key.mod & SDL_KMOD_ALT) ? 2 : 0) |
                                     ((ev.key.mod & SDL_KMOD_SHIFT) ? 4 : 0);
                if (ev.key.key == SDLK_RETURN && (held & 2) && !(held & 1)) {
                    hotkey_do(win, IM_HK_FULL, true, 1, &paused, minimized);
                    break;
                }
                for (int h = 0; h < IM_HOTKEYS; h++) {
                    const uint8_t need = g_map.hk_mod[h];
                    if (ev.key.key == g_map.hotkey[h] && (held & 3) == (need & 3) && (!(need & 4) || (held & 4))) {
                        if (need & 3) {
                            g_hk_eaten[ev.key.scancode] = true;   /* the game doesn't get the key */
                            g_hk_any_eaten = true;
                        }
                        hotkey_do(win, h, true, (held & 4) && !(need & 4) ? 2 : 1, &paused, minimized);
                        break;
                    }
                }
                break;
            }
            case SDL_EVENT_KEY_UP:
                if (ev.key.key == g_map.hotkey[IM_HK_TURBO]) {
                    hotkey_do(win, IM_HK_TURBO, false, 1, &paused, minimized);
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                /* the controller's shortcuts: Guide with a button (Guide
                 * alone, a tap: on its release, nothing else pressed), the
                 * touchpad, Share, Back with a button; and the menu with
                 * Back and Start */
                SDL_Gamepad *gp = SDL_GetGamepadFromID(ev.gbutton.which);
                const int b = ev.gbutton.button;
                int code = IM_HKP_NONE;
                if (b == SDL_GAMEPAD_BUTTON_GUIDE) {
                    g_guide_alone = true;
                } else if (b == SDL_GAMEPAD_BUTTON_TOUCHPAD) {
                    code = IM_HKP_TOUCHPAD;
                } else if (b == SDL_GAMEPAD_BUTTON_MISC1) {
                    code = IM_HKP_SHARE;
                } else if (pad_guide_held(gp)) {
                    code = IM_HKP_WITH + b;
                    g_guide_alone = false;
                } else if (gp && b != SDL_GAMEPAD_BUTTON_BACK && SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK)) {
                    code = IM_HKP_BACK_WITH + b;
                }
                for (int h = 0; code != IM_HKP_NONE && h < IM_HOTKEYS; h++) {
                    if (g_map.hk_pad[h] == code) {
                        g_pad_held_hk = h;
                        g_pad_held_code = code;
                        if (code >= IM_HKP_BACK_WITH) {
                            g_back_pad = gp;        /* the game doesn't get the button */
                            g_back_btn = b;
                        }
                        hotkey_do(win, h, true, 1, &paused, minimized);
                        break;
                    }
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                const int b = ev.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_GUIDE && g_guide_alone) {
                    g_guide_alone = false;
                    for (int h = 0; h < IM_HOTKEYS; h++) {
                        if (g_map.hk_pad[h] == IM_HKP_GUIDE) {
                            hotkey_do(win, h, true, 1, &paused, minimized);
                            hotkey_do(win, h, false, 1, &paused, minimized);
                            break;
                        }
                    }
                }
                /* a held shortcut (fast forward) ends with either button let go */
                if (g_pad_held_hk >= 0 &&
                    (b == SDL_GAMEPAD_BUTTON_GUIDE || IM_HKP_WITH + b == g_pad_held_code ||
                     IM_HKP_BACK_WITH + b == g_pad_held_code ||
                     (b == SDL_GAMEPAD_BUTTON_BACK && g_pad_held_code >= IM_HKP_BACK_WITH) ||
                     (b == SDL_GAMEPAD_BUTTON_TOUCHPAD && g_pad_held_code == IM_HKP_TOUCHPAD) ||
                     (b == SDL_GAMEPAD_BUTTON_MISC1 && g_pad_held_code == IM_HKP_SHARE))) {
                    hotkey_do(win, g_pad_held_hk, false, 1, &paused, minimized);
                    g_pad_held_hk = -1;
                }
                break;
            }
            case SDL_EVENT_JOYSTICK_ADDED:
                joystick_added(ev.jdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                pad_added(ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                pad_removed(ev.gdevice.which);
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                g_nw.width = ev.window.data1;
                g_nw.height = ev.window.data2;
                break;
            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_RESTORED:
                minimized = ev.type == SDL_EVENT_WINDOW_MINIMIZED;
                nc_kernel_set_paused(&g_core.kernel, paused || minimized);
                nc_audio_set_paused(paused || minimized);
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                focused = true;
                nc_audio_set_volume(g_volume);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                focused = false;
                if (g_mute_bg) {
                    nc_audio_set_volume(0);
                }
                break;
            default:
                break;
            }
        }
        nc_auto_tick();
        if (nc_auto_take_menu()) {
            ingame_menu(win, &paused, minimized);
        }
        update_pads(focused);
        state_request_poll();

        /* the GPU can't draw the game (no Vulkan device, a driver lacking
         * what it needs): the game ran on with its sound over a black
         * window, the reason only in the log */
        if (!told && nc_hle_fatal_kind() == NC_FATAL_DRIVER) {
            told = true;
            fatal_box("xbased", nc_hle_fatal());
            break;
        }
        const int state = SDL_GetAtomicInt(&g_core_state);
        if (state < 0) {
            if (g_core.kernel.relaunch) {
                run_again("relaunch", true);
            }
            if (g_core.kernel.quit) {
                if (g_core.kernel.quit_no_dash) {
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "xbased", T(S_NO_DASHBOARD), win);
                } else {
                    fx_out();
                }
                break;
            }
            if (!told) {
                told = true;
                const char *why = nc_hle_fatal();
                if (why) {
                    fatal_box("xbased", why);
                    break;
                }
            }
        }
        overlay_tick();
        if (minimized || g_nw.width <= 0 || !nc_present_frame(&g_nw)) {
            SDL_Delay(8); /* nothing to wait for the display on */
        }
#ifdef _WIN32
        if (g_frame && !IsWindow((HWND)g_frame)) {
            /* the frame's window went (its process fell), this run's with it:
             * not left running unseen, back to the dashboard */
            const char *why = hle_present_remote_why();
            nc_log("hakuX-app", "window: the frame's gone%s%s: leaving", why[0] ? ": " : "", why);
            exit_now(0);
        }
#endif
        if (g_frame && g_frame_hidden) {
            if (hle_present_remote_state() < 0) {
                g_frame_hidden = false;     /* no shared pictures: the window fills the frame, as before */
                nc_frame_fill(g_frame, win);
                nc_log("hakuX-app", "window: the frame's pictures can't be had: drawn in the window");
            } else if (hle_present_remote()) {
                /* the frame's mouse, as this window's (the in-game menu's pointer) */
                HleShareMouse ms[32];
                static int64_t at;
                const uint32_t n = hle_present_remote_mouse(ms, 32, &at);
                for (uint32_t k = 0; k < n; k++) {
                    SDL_Event e;
                    SDL_zero(e);
                    if (ms[k].type == 1) {
                        e.type = SDL_EVENT_MOUSE_MOTION;
                        e.motion.windowID = SDL_GetWindowID(win);
                        e.motion.x = (float)ms[k].x;
                        e.motion.y = (float)ms[k].y;
                        e.motion.xrel = g_frame_mx < 0 ? 4.0f : (float)ms[k].x - g_frame_mx;
                        e.motion.yrel = g_frame_my < 0 ? 4.0f : (float)ms[k].y - g_frame_my;
                        g_frame_mx = (float)ms[k].x;
                        g_frame_my = (float)ms[k].y;
                    } else if (ms[k].type == 2 || ms[k].type == 3) {
                        e.type = ms[k].type == 2 ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                        e.button.windowID = SDL_GetWindowID(win);
                        e.button.button = (Uint8)ms[k].button;
                        e.button.down = ms[k].type == 2;
                        e.button.clicks = 1;
                        e.button.x = (float)ms[k].x;
                        e.button.y = (float)ms[k].y;
                    } else {
                        e.type = SDL_EVENT_MOUSE_WHEEL;
                        e.wheel.windowID = SDL_GetWindowID(win);
                        e.wheel.y = (float)ms[k].wheel;
                    }
                    SDL_PushEvent(&e);
                }
                hle_present_remote_cursor(SDL_CursorVisible());
            }
        }
        static bool raised;
        if (g_frame && !raised && hle_present_count() > 0) {
            raised = true;
            nc_frame_raise(g_frame, win);   /* over the run before's, which goes next */
        }
        if (g_frame) {
            const bool m = nc_frame_minimized(g_frame);
            if (m != minimized) {
                minimized = m;      /* a child window hears nothing of its frame's minimizing */
                nc_kernel_set_paused(&g_core.kernel, paused || minimized);
                nc_audio_set_paused(paused || minimized);
            }
        }
#ifndef _WIN32
        if (g_ready_fd >= 0 && hle_present_count() > 0) {
            if (write(g_ready_fd, "1", 1) < 0) {
            }
            close(g_ready_fd);  /* drawn: the run before may go */
            g_ready_fd = -1;
        }
#else
        if (g_ready_ev && hle_present_count() > 0) {
            SetEvent(g_ready_ev);   /* drawn: the run before may go */
            CloseHandle(g_ready_ev);
            g_ready_ev = NULL;
        }
#endif
    }
    nc_log("hakuX-app", "native core: leaving");
    exit_now(0); /* the core thread has no stop yet */
}
