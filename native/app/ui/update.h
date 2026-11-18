/*
 * xbased front end — xbased's updates: the latest release on GitHub, checked
 * as the dashboard starts, and installed over the running copy.
 */
#ifndef NATIVE_APP_UI_UPDATE_H
#define NATIVE_APP_UI_UPDATE_H

#include <stdbool.h>

typedef enum UpdState {
    UPD_IDLE,        /* not checked, or nothing newer */
    UPD_CHECKING,
    UPD_AVAILABLE,   /* a newer release (upd_version) */
    UPD_INSTALLING,  /* its download and install (upd_install_start) */
    UPD_INSTALLED,   /* ready: upd_relaunch starts it */
    UPD_FAILED,      /* the install failed: the release's page is the way left */
} UpdState;

/* What an update left behind (the replaced executable): removed. */
void upd_cleanup(void);
/* Asks GitHub for the latest release, on a thread; current: this version. */
void upd_check_start(const char *current);
UpdState upd_state(void);
const char *upd_version(void);
/* The release's page (the browser's way to update). */
const char *upd_page(void);
/* Whether this copy can replace itself: the Windows folder writable, or an
 * AppImage (Linux). Otherwise the page is the way. */
bool upd_can_install(void);
/* Downloads the release's package and installs it over this copy, on a
 * thread (UPD_INSTALLED or UPD_FAILED). */
void upd_install_start(void);
/* Starts the installed version; the caller quits. */
bool upd_relaunch(void);

#endif
