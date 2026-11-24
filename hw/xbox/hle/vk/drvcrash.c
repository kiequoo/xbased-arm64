/*
 * xbased HLE GPU — a driver crash in a pipeline build (see drvcrash.h).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/vk/drvcrash.h"

#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/host.h"
#include "hw/xbox/hle/vk/vkdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_id[96];               /* "vendor:device:driver" of the GPU */
static char g_path[320];            /* <base>/gpu_precision.txt */
static __thread int t_building;     /* in a driver call that builds */
static __thread int t_linking;      /* in one from pipeline libraries (gpl.c) */
static __thread uint64_t t_pipe;    /* the pipeline this thread builds */
#define MAX_BAD_PIPES 64
static uint64_t g_bad_pipes[MAX_BAD_PIPES];
static unsigned g_nbad_pipes;

/* The line of id: its verdict and reason (false: none) */
static bool verdict_read_id(const char *id, char *verdict, size_t vn, char *reason, size_t rn)
{
    FILE *f = g_path[0] ? fopen(g_path, "r") : NULL;
    if (!f) {
        return false;
    }
    char line[512];
    bool got = false;
    const size_t il = strlen(id);
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, id, il) || line[il] != ' ') {
            continue;
        }
        const char *v = line + il + 1;
        const char *sp = strchr(v, ' ');
        snprintf(verdict, vn, "%.*s", sp ? (int)(sp - v) : (int)strlen(v), v);
        snprintf(reason, rn, "%s", sp ? sp + 1 : "");
        got = true;         /* (the last line of it wins) */
    }
    fclose(f);
    return got;
}

/* id's verdict kept: the file's other lines, then this one */
static void verdict_write_id(const char *id, const char *verdict, const char *reason)
{
    char *keep = NULL;
    size_t kn = 0;
    FILE *f = fopen(g_path, "r");
    if (f) {
        char line[512];
        const size_t il = strlen(id);
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, id, il) && line[il] == ' ') {
                continue;
            }
            const size_t l = strlen(line);
            char *n = realloc(keep, kn + l + 1);
            if (!n) {
                break;
            }
            keep = n;
            memcpy(keep + kn, line, l + 1);
            kn += l;
        }
        fclose(f);
    }
    f = fopen(g_path, "w");
    if (f) {
        if (keep) {
            fputs(keep, f);
        }
        fprintf(f, "%s %s %s\n", id, verdict, reason);
        fclose(f);
    }
    free(keep);
}

void hle_drvcrash_init(VkPhysicalDevice phys)
{
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(phys, &p);
    snprintf(g_id, sizeof(g_id), "%04x:%04x:%08x", p.vendorID, p.deviceID, p.driverVersion);
    snprintf(g_path, sizeof(g_path), "%sgpu_precision.txt", hle_host_base_path());
    FILE *f = fopen(g_path, "r");
    if (!f) {
        return;
    }
    char line[512], pre[128];
    snprintf(pre, sizeof(pre), "%s/pipe:", g_id);
    const size_t pl = strlen(pre);
    while (fgets(line, sizeof(line), f) && g_nbad_pipes < MAX_BAD_PIPES) {
        unsigned long long key;
        char v[16];
        if (!strncmp(line, pre, pl) && sscanf(line + pl, "%llx %15s", &key, v) == 2 && !strcmp(v, "off")) {
            g_bad_pipes[g_nbad_pipes++] = key;
        }
    }
    fclose(f);
    if (g_nbad_pipes) {
        hle_log("xbased-hle-back", "drivers: %u pipeline(s) not built on this GPU and driver (they crashed its "
                "compiler in an earlier run): their draws are skipped", g_nbad_pipes);
    }
}

void hle_drvcrash_build_enter(void)
{
    t_building++;
}

void hle_drvcrash_build_leave(void)
{
    t_building--;
}

void hle_drvcrash_link_enter(void)
{
    t_linking++;
}

void hle_drvcrash_link_leave(void)
{
    t_linking--;
}

void hle_drvcrash_pipe_set(uint64_t key)
{
    t_pipe = key;
}

bool hle_drvcrash_pipe_blocked(uint64_t key)
{
    for (unsigned i = 0; i < g_nbad_pipes; i++) {
        if (g_bad_pipes[i] == key) {
            return true;
        }
    }
    return false;
}

bool hle_drvcrash_gpl_blocked(char *why, size_t wn)
{
    char id[128], v[32];
    snprintf(id, sizeof(id), "%s/gpl", g_id);
    return verdict_read_id(id, v, sizeof(v), why, wn) && !strcmp(v, "off");
}

void hle_drvcrash_crashed(void)
{
    if (!g_path[0]) {
        return;
    }
    if (t_building > 0 && t_pipe) {
        char id[160];
        snprintf(id, sizeof(id), "%s/pipe:%016llx", g_id, (unsigned long long)t_pipe);
        verdict_write_id(id, "off", "the driver crashed building it in an earlier run");
    }
    if (t_linking > 0) {
        char id[128];
        snprintf(id, sizeof(id), "%s/gpl", g_id);
        verdict_write_id(id, "off", "the driver crashed linking a shader from its parts in an earlier run");
    }
}
