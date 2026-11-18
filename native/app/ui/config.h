/*
 * xbased front end — settings files (config.c). One option a line, as the
 * command line takes them ("scale=2", "uncap", "mod=60fps"); '#' comments.
 * xbased.ini holds the global ones, games/<TITLE ID>.<version>.ini a game's
 * (the game's override the global, the command line both).
 */
#ifndef NATIVE_APP_UI_CONFIG_H
#define NATIVE_APP_UI_CONFIG_H

#include <stdbool.h>

#define CFG_MAX 256

typedef struct Config {
    char path[1024];
    int n;
    char key[CFG_MAX][48];
    char val[CFG_MAX][464];     /* "" for a flag ("uncap"); "1" is written as the flag */
    bool flag[CFG_MAX];
    char comments[2048];        /* the file's leading comment lines, kept */
} Config;

void cfg_load(Config *c, const char *path);
bool cfg_save(const Config *c);
/* The file written by something else (a text editor, a game) since this
 * process last read or wrote it. */
bool cfg_changed_outside(const Config *c);
const char *cfg_get(const Config *c, const char *key);    /* NULL: unset */
int cfg_int(const Config *c, const char *key, int def);
bool cfg_bool(const Config *c, const char *key, bool def);
void cfg_set(Config *c, const char *key, const char *val);
void cfg_set_int(Config *c, const char *key, int v);
void cfg_set_bool(Config *c, const char *key, bool v);
void cfg_unset(Config *c, const char *key);
/* Asynchronous shaders and the prebuild became off by default (1.0.73): a
 * setting of either from before goes, once (a mark keeps one made since).
 * True when one went. */
bool cfg_shaders_migrate(Config *c);
/* Keys that may repeat (mod=, cheat=): all their values. */
int cfg_all(const Config *c, const char *key, const char **out, int max);
void cfg_add(Config *c, const char *key, const char *val);
void cfg_remove_value(Config *c, const char *key, const char *val);

/* Where the emulator keeps its data (xbased.ini, hdd, states, games/,
 * covers/): beside the executable (Windows, or a portable.txt there), else
 * the user's data folder (Linux: ~/.local/share/xbased). Ends in a separator. */
const char *cfg_data_dir(void);
/* The global settings' file: xbased.ini in the data folder, or the one the
 * command line named (-c, --config: XBASED_CONFIG, which the game's process
 * and the runs after it inherit). */
const char *cfg_global_path(void);
/* The hard disk's cache partitions (X, Y and Z: Partition3 to 5 and their
 * heads, raw-partition3 to 5.bin) emptied, as a console's cache cleared:
 * the titles copy their files from the disc there again (#308). hdd: the
 * hard disk's folder, NULL the data folder's. The files removed, or -1. */
int cfg_clear_game_cache(const char *hdd);
/* Beside the executable (fonts, mods). */
const char *cfg_exe_dir(void);
/* Runs a helper program (curl, tar) to its end, with no window of its own
 * (a console's, on Windows); true when it exits with 0. */
bool cfg_run(const char *const *args);

#endif
