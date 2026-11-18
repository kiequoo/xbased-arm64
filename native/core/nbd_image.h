/*
 * xbased native core — a disc image served over NBD (nbd://host[:port][/export]),
 * read-only: GDOX's physical discs (it serves a drive's disc to xemu that
 * way), or any NBD server's image.
 *
 * The disc's descriptor is a small marker file (nc_nbd_marker_write: a magic
 * line and the URI), so every dup of it, as the core and the front end open
 * the disc, finds the one connection again (nc_nbd_attach): an NBD server
 * may take one client at a time.
 */
#ifndef NATIVE_CORE_NBD_IMAGE_H
#define NATIVE_CORE_NBD_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "nbd://..." */
bool nc_nbd_is_uri(const char *path);
/* The marker for uri at path (created or replaced). */
bool nc_nbd_marker_write(const char *path, const char *uri);
/* fd a marker: its URI's connection made (or shared) and fd registered. */
bool nc_nbd_attach(int fd);
void nc_nbd_detach(int fd);
bool nc_nbd_is(int fd);
/* len bytes of the image at off; zeros past its end. */
bool nc_nbd_read(int fd, void *buf, size_t len, uint64_t off);
uint64_t nc_nbd_size(int fd);

#ifdef __cplusplus
}
#endif

#endif
