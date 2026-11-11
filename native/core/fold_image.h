/*
 * hakuX native core — a game folder as a disc (fold_image.c): an extracted
 * game (its default.xbe and the files beside it) read as the XDVDFS image
 * they would make, its directory tables built in memory and each file's
 * sectors read from the file itself.
 */
#ifndef NATIVE_CORE_FOLD_IMAGE_H
#define NATIVE_CORE_FOLD_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An open descriptor of a game folder's default.xbe: the folder as a disc,
 * registered by the descriptor, so the disc's reads (nc_fold_read on it) go
 * through it. False: not an XBE (a disc image), or a folder that can't be
 * read. */
bool nc_fold_attach(int fd);
void nc_fold_detach(int fd);
/* fd registered as a folder */
bool nc_fold_is(int fd);
/* len bytes of the image at off; zeros past its end. False: a file that
 * can't be read. Any thread. */
bool nc_fold_read(int fd, void *buf, size_t len, uint64_t off);

/* A path the front end may hand over for a game: a folder holding a
 * default.xbe (or that file itself) gives the default.xbe's path in out,
 * true. Anything else (a disc image) false, out untouched. */
bool nc_fold_xbe_path(const char *path, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
