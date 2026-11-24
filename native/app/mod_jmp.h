/*
 * xbased — Jay's Magic Patcher's patches (.JMP, mods/README.md): a text
 * file of patch records for an Xbox executable, from the collections
 * shared online (#391). A record finds bytes and replaces them (the first
 * place they are), writes bytes at a file offset, or appends bytes to the
 * file; the header names the game (version=<title ID> ...) and may list the
 * CRC-32s of the executables it was made for ({302E6C4F}).
 */
#ifndef NATIVE_APP_MOD_JMP_H
#define NATIVE_APP_MOD_JMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct JmpInfo {
    uint32_t title_id;          /* version='s, 0 when it names none */
    char notes[256];            /* notes= (its "<br>"s as spaces) */
    char author[96];
} JmpInfo;

/* The patch at path for default.xbe (x, len bytes; title_id its
 * certificate's): -1 when it isn't for this executable (why: another game,
 * another build by its CRC-32s, a record whose bytes aren't there, an
 * append record), else how many records it applies, its lines written to
 * out (NULL: only checked) as "patch" (on) or "patchoff" lines of the mod
 * id. Every record goes in, in order, or none: a patch made for another
 * build that fits in part would change only some of what it changes. */
int nc_jmp_lines(const char *path, const uint8_t *x, uint32_t len, uint32_t title_id, FILE *out, const char *id,
                 bool on, JmpInfo *info, char *why, size_t wn);

/* The title ID a patch names (version=), 0 when none: a .JMP in mods/
 * itself is the game's it names. */
uint32_t nc_jmp_title(const char *path);

/* Its offset records as an IPS patch (for mod_fit.h's check of where they
 * land in code), NULL when it has none; *crc: it lists the CRC-32s of the
 * executables it was made for (nc_jmp_lines checks those). free() it. */
uint8_t *nc_jmp_offsets_ips(const char *path, size_t *len, bool *crc);

/* A file name ending in .jmp (any case). */
bool nc_jmp_name(const char *name);

#endif
