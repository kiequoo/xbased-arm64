/*
 * xbased — Jay's Magic Patcher's .JMP patches (mod_jmp.h).
 *
 * The format (the collections' README): a "#" first line, then header
 * lines "key=value" (system, game-title, region, version, author, notes),
 * then records, each after a "#" comment line:
 *   two hex lines, the bytes to find and what replaces them (the first
 *     place they are in the file, as the file is after the records before);
 *   "<offset>:<hex>", the bytes written at that file offset (0x120, 120,
 *     00000120 the same);
 *   "APPEND:<hex>", bytes added at the file's end (an executable "exploded"
 *     for room: not taken here, the run reads its sections from the disc).
 * A comment with "[-]" in it turns off the records under it.
 *
 * Each record becomes the manifest's patch lines at the addresses its bytes
 * load at (the headers at the base, a section's raw data at its address):
 * the found bytes checked (old -> new), as the widescreen database's are.
 */
#include "native/app/mod_jmp.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>

#define JMP_LINE_BYTES 128      /* a manifest line's bytes at most (core/mods.c takes 512) */

typedef struct {
    int kind;                   /* 0 find and replace, 1 at an offset, 2 append */
    uint32_t off;
    uint8_t *find, *repl;       /* (offset, append: repl only) */
    uint32_t n;
} JmpRec;

bool nc_jmp_name(const char *name)
{
    const size_t n = strlen(name);
    return n > 4 && !strcasecmp(name + n - 4, ".jmp");
}

/* the line's hex digits as bytes (blanks skipped); -1 if anything else is in
 * it or an odd digit is left */
static long hex_line(const char *s, uint8_t *out)
{
    long n = 0;
    int half = -1;
    for (; *s; s++) {
        if (*s == ' ' || *s == '\t') {
            continue;
        }
        if (!isxdigit((unsigned char)*s)) {
            return -1;
        }
        const int v = isdigit((unsigned char)*s) ? *s - '0' : (tolower((unsigned char)*s) - 'a' + 10);
        if (half < 0) {
            half = v;
        } else {
            if (out) {
                out[n] = (uint8_t)(half << 4 | v);
            }
            n++;
            half = -1;
        }
    }
    return half < 0 ? n : -1;
}

static uint8_t *hex_dup(const char *s, uint32_t *n)
{
    const long k = hex_line(s, NULL);
    if (k <= 0) {
        return NULL;
    }
    uint8_t *b = malloc((size_t)k);
    if (b) {
        hex_line(s, b);
        *n = (uint32_t)k;
    }
    return b;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
    return s;
}

/* "4D530064 (MS-100) {302E6C4F} {...}": the title ID (8 hex digits first)
 * and the CRC-32s in braces */
static void version_line(const char *v, uint32_t *tid, uint32_t *crcs, int *ncrc)
{
    *tid = 0;
    size_t k = 0;
    while (isxdigit((unsigned char)v[k])) {
        k++;
    }
    if (k == 8 && (!v[k] || v[k] == ' ' || v[k] == '\t' || v[k] == '(')) {
        *tid = (uint32_t)strtoul(v, NULL, 16);
    }
    for (const char *p = strchr(v, '{'); p && *ncrc < 16; p = strchr(p + 1, '{')) {
        size_t d = 0;
        while (isxdigit((unsigned char)p[1 + d])) {
            d++;
        }
        if (d == 8 && p[9] == '}') {
            crcs[(*ncrc)++] = (uint32_t)strtoul(p + 1, NULL, 16);
        }
    }
}

/* region='s releases as the certificate's game regions (1 North America,
 * 2 Japan, 4 the rest; 7 any: "Global", "All Regions"); 0 unknown */
static uint32_t region_mask(const char *v)
{
    char u[128];
    size_t n = 0;
    for (; v[n] && n + 1 < sizeof(u); n++) {
        u[n] = (char)toupper((unsigned char)v[n]);
    }
    u[n] = 0;
    if (strstr(u, "GLOBAL") || strstr(u, "ALL") || strstr(u, "MULTI") || strstr(u, "WORLD") || strstr(u, "ANY")) {
        return 7;
    }
    uint32_t m = 0;
    for (const char *p = u; (p = strstr(p, "NTSC")); p += 4) {
        m |= p[4] == 'J' || (p[4] == '-' && p[5] == 'J') ? 2u : 1u;
    }
    if (strstr(u, "USA") || strstr(u, "US_") || strstr(u, "CANADA") || strstr(u, "NORTH")) {
        m |= 1u;
    }
    if (strstr(u, "JPN") || strstr(u, "JAP") || strstr(u, "ASIA")) {
        m |= 2u;
    }
    if (strstr(u, "PAL") || strstr(u, "EUR") || strstr(u, "ELSEWHERE") || strstr(u, "AUS")) {
        m |= 4u;
    }
    return m;
}

typedef struct {
    JmpInfo info;
    uint32_t regions;           /* region='s (region_mask) */
    uint32_t crcs[16];
    int ncrc;
    JmpRec *rec;
    int n, cap;
    bool bad;                   /* a line that is none of the records' */
    int bad_line;
} Jmp;

static void jmp_free(Jmp *j)
{
    for (int i = 0; i < j->n; i++) {
        free(j->rec[i].find);
        free(j->rec[i].repl);
    }
    free(j->rec);
}

static JmpRec *add(Jmp *j)
{
    if (j->n == j->cap) {
        const int cap = j->cap ? j->cap * 2 : 32;
        JmpRec *r = realloc(j->rec, (size_t)cap * sizeof(*r));
        if (!r) {
            return NULL;
        }
        j->rec = r;
        j->cap = cap;
    }
    JmpRec *r = &j->rec[j->n++];
    memset(r, 0, sizeof(*r));
    return r;
}

static bool jmp_parse(const char *path, Jmp *j)
{
    memset(j, 0, sizeof(*j));
    size_t len = 0;
    char *t = SDL_LoadFile(path, &len);
    if (!t) {
        return false;
    }
    bool off = false, head = true;
    char *find = NULL;  /* a find line waiting for its replacement */
    int line_no = 0;
    for (char *s = t, *e; s && *s; s = e) {
        e = strchr(s, '\n');
        if (e) {
            *e++ = 0;
        }
        line_no++;
        char *l = trim(s);
        if (!l[0]) {
            continue;
        }
        if (l[0] == '#') {
            if (find) {
                j->bad = true;      /* a find line with no replacement */
                j->bad_line = line_no;
            }
            find = NULL;
            if (line_no > 1) {
                head = false;
                off = strstr(l, "[-]") != NULL;
            }
            continue;
        }
        char *eq = strchr(l, '=');
        if (head && eq) {
            *eq = 0;
            const char *k = trim(l), *v = trim(eq + 1);
            if (!strcasecmp(k, "version")) {
                version_line(v, &j->info.title_id, j->crcs, &j->ncrc);
            } else if (!strcasecmp(k, "notes")) {
                snprintf(j->info.notes, sizeof(j->info.notes), "%s", v);
                for (char *b; (b = strstr(j->info.notes, "<br>"));) {
                    *b = ' ';
                    memmove(b + 1, b + 4, strlen(b + 4) + 1);
                }
            } else if (!strcasecmp(k, "author")) {
                snprintf(j->info.author, sizeof(j->info.author), "%s", v);
            } else if (!strcasecmp(k, "region")) {
                j->regions = region_mask(v);
            }
            continue;
        }
        head = false;
        char *colon = strchr(l, ':');
        JmpRec r = {0};
        bool ok = false;
        if (colon) {
            *colon = 0;
            const char *a = trim(l);
            r.repl = hex_dup(colon + 1, &r.n);
            if (!strcasecmp(a, "APPEND")) {
                r.kind = 2;
                ok = r.repl != NULL;
            } else {
                char *end = NULL;
                const unsigned long o = strtoul(a, &end, 16);  /* "0x120" and "120" alike */
                r.kind = 1;
                r.off = (uint32_t)o;
                ok = r.repl && end && !*end && a[0];
            }
        } else if (!find) {
            find = l;
            continue;
        } else {
            uint32_t nf = 0;
            r.find = hex_dup(find, &nf);
            r.repl = hex_dup(l, &r.n);
            ok = r.find && r.repl && nf == r.n;     /* (one length: a replacement, not an insert) */
            find = NULL;
        }
        if (!ok) {
            free(r.find);
            free(r.repl);
            if (!j->bad) {
                j->bad = true;
                j->bad_line = line_no;
            }
            continue;
        }
        if (off) {
            free(r.find);
            free(r.repl);
            continue;
        }
        JmpRec *p = add(j);
        if (p) {
            *p = r;
        } else {
            free(r.find);
            free(r.repl);
        }
    }
    if (find && !j->bad) {
        j->bad = true;
        j->bad_line = line_no;
    }
    SDL_free(t);
    return true;
}

uint32_t nc_jmp_title(const char *path)
{
    Jmp j;
    if (!jmp_parse(path, &j)) {
        return 0;
    }
    const uint32_t tid = j.info.title_id;
    jmp_free(&j);
    return tid;
}

uint8_t *nc_jmp_offsets_ips(const char *path, size_t *len, bool *crc)
{
    Jmp j;
    if (!jmp_parse(path, &j)) {
        return NULL;
    }
    *crc = j.ncrc > 0;
    size_t cap = 8;
    for (int i = 0; i < j.n; i++) {
        if (j.rec[i].kind == 1) {
            cap += j.rec[i].n + (j.rec[i].n / 0xFFFF + 1) * 5;
        }
    }
    uint8_t *p = cap > 8 ? malloc(cap) : NULL;
    size_t at = 0;
    if (p) {
        memcpy(p, "PATCH", 5);
        at = 5;
        for (int i = 0; i < j.n; i++) {
            const JmpRec *r = &j.rec[i];
            for (uint32_t done = 0; r->kind == 1 && done < r->n;) {
                const uint32_t o = r->off + done, k = r->n - done > 0xFFFF ? 0xFFFF : r->n - done;
                if (o > 0xFFFFFF) {
                    free(p);    /* past what IPS can say: left unchecked */
                    jmp_free(&j);
                    return NULL;
                }
                const uint8_t h[5] = {(uint8_t)(o >> 16), (uint8_t)(o >> 8), (uint8_t)o, (uint8_t)(k >> 8), (uint8_t)k};
                memcpy(p + at, h, 5);
                memcpy(p + at + 5, r->repl + done, k);
                at += 5 + k;
                done += k;
            }
        }
        memcpy(p + at, "EOF", 3);
        at += 3;
    }
    jmp_free(&j);
    *len = at;
    return p;
}

static uint32_t crc32_of(const uint8_t *p, uint32_t n)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A file offset's address and how many bytes from it stay in its range (the
 * headers', to the first section's data, or a section's raw data: as
 * core/mods.c's xbe_offset maps them back); 0 when it loads nowhere. */
static uint32_t to_va(const uint8_t *x, uint32_t len, uint32_t o, uint32_t *va)
{
    const uint32_t base = rd32(x + 0x104), hsize = rd32(x + 0x108), nsec = rd32(x + 0x11C);
    const uint32_t secs = rd32(x + 0x120) - base;
    uint32_t hdr_end = 0x1000;
    for (uint32_t k = 0; k < nsec && k < 256 && secs + (k + 1) * 0x38 <= len; k++) {
        const uint32_t raw = rd32(x + secs + k * 0x38 + 0xC);
        hdr_end = raw < hdr_end ? raw : hdr_end;
    }
    hdr_end = hdr_end > hsize ? hdr_end : hsize;
    if (o < hdr_end) {
        *va = base + o;
        return hdr_end - o;
    }
    for (uint32_t k = 0; k < nsec && k < 256 && secs + (k + 1) * 0x38 <= len; k++) {
        const uint8_t *sh = x + secs + k * 0x38;
        const uint32_t sva = rd32(sh + 4), raw = rd32(sh + 0xC), rsize = rd32(sh + 0x10);
        if (o >= raw && o - raw < rsize) {
            *va = sva + (o - raw);
            return rsize - (o - raw);
        }
    }
    return 0;
}

static long find_bytes(const uint8_t *x, uint32_t len, const uint8_t *f, uint32_t n)
{
    for (uint32_t o = 0; n && o + n <= len; o++) {
        if (x[o] == f[0] && !memcmp(x + o, f, n)) {
            return (long)o;
        }
    }
    return -1;
}

/* the bytes at file offset o as patch lines (old -> new when old); false if
 * some of them load nowhere */
static bool emit(FILE *out, const uint8_t *x, uint32_t len, uint32_t o, const uint8_t *old, const uint8_t *neu,
                 uint32_t n, const char *id, bool on)
{
    for (uint32_t done = 0; done < n;) {
        uint32_t va = 0;
        uint32_t k = to_va(x, len, o + done, &va);
        if (!k) {
            return false;
        }
        k = k < n - done ? k : n - done;
        k = k < JMP_LINE_BYTES ? k : JMP_LINE_BYTES;
        if (out) {
            fprintf(out, "%s\tdefault.xbe\t%s\t0x%08X:", on ? "patch" : "patchoff", id, va);
            if (old) {
                for (uint32_t i = 0; i < k; i++) {
                    fprintf(out, " %02X", old[done + i]);
                }
                fprintf(out, " ->");
            }
            for (uint32_t i = 0; i < k; i++) {
                fprintf(out, " %02X", neu[done + i]);
            }
            fprintf(out, "\n");
        }
        done += k;
    }
    return true;
}

int nc_jmp_lines(const char *path, const uint8_t *x, uint32_t len, uint32_t title_id, FILE *out, const char *id,
                 bool on, JmpInfo *info, char *why, size_t wn)
{
    Jmp j;
    if (!jmp_parse(path, &j)) {
        snprintf(why, wn, "it can't be read");
        return -1;
    }
    if (info) {
        *info = j.info;
    }
    int lines = -1;
    uint8_t *buf = NULL;
    if (j.bad) {
        snprintf(why, wn, "line %d isn't a patch record", j.bad_line);
        goto done;
    }
    if (!x || len < 0x1000) {
        snprintf(why, wn, "no default.xbe");
        goto done;
    }
    if (j.info.title_id && j.info.title_id != title_id) {
        snprintf(why, wn, "made for another game (%08X)", j.info.title_id);
        goto done;
    }
    if (j.ncrc) {
        const uint32_t c = crc32_of(x, len);
        bool any = false;
        for (int i = 0; i < j.ncrc; i++) {
            any |= j.crcs[i] == c;
        }
        if (!any) {
            snprintf(why, wn, "made for another build of the game (this default.xbe's CRC-32 is %08X)", c);
            goto done;
        }
    }
    if (!j.n) {
        snprintf(why, wn, "no records");
        goto done;
    }
    /* offset records write blind: with no CRC-32 to tell the build, the
     * releases its region names (Castlevania: Curse of Darkness's NTSC
     * widescreen patch wrote over other code on the European executable,
     * where its own lands elsewhere) */
    bool offsets = false;
    for (int i = 0; i < j.n; i++) {
        offsets |= j.rec[i].kind == 1;
    }
    const uint32_t cert = rd32(x + 0x118) - rd32(x + 0x104);
    const uint32_t sold = cert < len && len - cert >= 0xA4 ? rd32(x + cert + 0xA0) & 7u : 0;
    /* (the file name's region too, "Game {kind} (PAL) [author]": a header
     * left saying another, Blade II's PAL widescreen patch's "NTSC") */
    uint32_t regions = j.regions;
    const char *base = path;
    for (const char *c = path; *c; c++) {
        if (*c == '/' || *c == '\\') {
            base = c + 1;
        }
    }
    const char *g = strchr(base, '}');
    g = strchr(g ? g : base, '(');
    const char *ge = g ? strchr(g, ')') : NULL;
    if (ge && ge - g < 120) {
        char grp[128];
        snprintf(grp, sizeof(grp), "%.*s", (int)(ge - g - 1), g + 1);
        regions |= region_mask(grp);
    }
    if (offsets && !j.ncrc && regions && sold && !(regions & sold)) {
        snprintf(why, wn, "its offsets are another region's release's (%s)",
                 regions == 1 ? "North America" : regions == 2 ? "Japan" : regions == 3 ? "NTSC" : "other");
        goto done;
    }
    /* the records in order over a copy, as the patcher writes the file:
     * a record finds bytes the ones before it may have written */
    buf = malloc(len);
    if (!buf) {
        snprintf(why, wn, "no memory");
        goto done;
    }
    memcpy(buf, x, len);
    for (int i = 0; i < j.n; i++) {
        const JmpRec *r = &j.rec[i];
        if (r->kind == 2) {
            snprintf(why, wn, "it appends to default.xbe (an exploded executable), which xbased doesn't do");
            goto done;
        }
        long o = r->kind == 1 ? (long)r->off : find_bytes(buf, len, r->find, r->n);
        if (o < 0) {
            snprintf(why, wn, "record %d's bytes aren't in this default.xbe", i + 1);
            goto done;
        }
        if ((uint32_t)o + r->n > len || (uint32_t)o + r->n < (uint32_t)o) {
            snprintf(why, wn, "record %d is past the end of default.xbe", i + 1);
            goto done;
        }
        /* checked first (lines only once every record fits) */
        if (!emit(NULL, buf, len, (uint32_t)o, NULL, r->repl, r->n, id, on)) {
            snprintf(why, wn, "record %d is in a part of default.xbe that isn't loaded", i + 1);
            goto done;
        }
        memcpy(buf + o, r->repl, r->n);
    }
    /* again from the disc's bytes, the lines written */
    memcpy(buf, x, len);
    lines = 0;
    for (int i = 0; i < j.n; i++) {
        const JmpRec *r = &j.rec[i];
        const long o = r->kind == 1 ? (long)r->off : find_bytes(buf, len, r->find, r->n);
        uint8_t *old = r->kind == 0 ? r->find : NULL;   /* an offset's, as an IPS record: unchecked */
        emit(out, buf, len, (uint32_t)o, old, r->repl, r->n, id, on);
        memcpy(buf + o, r->repl, r->n);
        lines++;
    }
done:
    free(buf);
    jmp_free(&j);
    return lines;
}
