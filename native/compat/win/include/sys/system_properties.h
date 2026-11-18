/*
 * xbased on Windows: Android's system properties, for the debug.xbased.*
 * switches. Read from props.ini beside the executable (name=value lines,
 * read again when it changes) or from the environment, the name upper-cased
 * with '.' as '_' (debug.xbased.perf: DEBUG_XBASED_PERF). compat/win/win.c.
 */
#ifndef NC_WIN_SYSTEM_PROPERTIES_H
#define NC_WIN_SYSTEM_PROPERTIES_H

#define PROP_VALUE_MAX 92
#define PROP_NAME_MAX 32

#ifdef __cplusplus
extern "C" {
#endif
int __system_property_get(const char *name, char *value);
/* A value set while running, over props.ini's (NULL: unset). */
void nc_win_prop_set(const char *name, const char *value);
#ifdef __cplusplus
}
#endif

#endif
