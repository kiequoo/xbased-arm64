/*
 * xbased HLE GPU — pipelines linked from libraries (gpl.h).
 *
 * Four tables of parts by key, shared by the render thread and the pipeline
 * workers under one lock (held for lookups only: a part is built outside
 * it, and one built twice meanwhile is let go). Parts live for the session:
 * a link takes under a millisecond, and the next pipeline of the same vertex
 * program or pixel stage needs them again. Fast links only (no optimizing
 * across the stages): the stages' code is what the driver built for each.
 *
 * A stage's shader module is made (HleGplDesc.make) only when its part is
 * built: a pipeline linked from parts the tables hold costs no SPIR-V. The
 * render thread's builds make their pixel part on a thread of their own
 * meanwhile (its SPIR-V too): a new stand-in's two parts took the driver tens
 * of milliseconds each, one after the other, at the draw that first needed it.
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"

#include "hw/xbox/hle/vk/gpl.h"
#include "hw/xbox/hle/vk/precision.h"
#include "hw/xbox/hle/vk/pcache.h"
#include "hw/xbox/hle/vk/vkdev.h"

#define T (g_hvk.t)
#define PART_CAP 16384          /* per kind, a power of two; past it the pipeline is built whole */

enum { PART_VI, PART_PRE, PART_FS, PART_OUT, PART_N };

typedef struct Part {
    uint64_t key;
    VkPipeline lib;
} Part;

static struct {
    int on;                     /* -1 not decided */
    HleMutex mu;
    HleCond cv;                 /* a part built (PART_BUILDING no longer) */
    Part *tab[PART_N];
    unsigned n[PART_N];
} g_gl = { .on = -1 };

/* Part.lib while a thread builds it (the others wait for it rather than build
 * it again: vk/draw.c's look-ahead builds a frame's new parts ahead of its
 * draws), and where its build failed (the others build their own, not kept) */
#define PART_BUILDING ((VkPipeline)(uintptr_t)1)
#define PART_FAILED   ((VkPipeline)(uintptr_t)2)

static const VkGraphicsPipelineLibraryFlagsEXT k_part_flag[PART_N] = {
    VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
    VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
    VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
    VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
};

bool hle_gpl_on(void)
{
    if (g_gl.on < 0) {
        const char *e = getenv("XBASED_HLE_GPL");
        bool on = g_hvk.caps.gpl && g_hvk.caps.vids && g_hvk.caps.eds1 && g_hvk.caps.eds2 &&
                  !(e && *e && atoi(e) == 0);
        char why[200];
        if (on && hle_precision_gpl_blocked(why, sizeof(why))) {
            hle_log("xbased-hle-pipe", "pipelines linked from libraries: off for this GPU and driver (%s)", why);
            on = false;
        }
        if (on) {
            /* a link that would compile (no fast linking) is no gain */
            VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gp = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT };
            VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               .pNext = &gp };
            vkGetPhysicalDeviceProperties2(g_hvk.phys, &p2);
            on = gp.graphicsPipelineLibraryFastLinking;
        }
        if (on) {
            hle_mutex_init(&g_gl.mu);
            hle_cond_init(&g_gl.cv);
            for (unsigned k = 0; k < PART_N; k++) {
                g_gl.tab[k] = g_new0(Part, PART_CAP);
            }
        }
        hle_log("xbased-hle-pipe", "pipelines linked from libraries: %s", on ? "on" : "off");
        g_gl.on = on;
    }
    return g_gl.on > 0;
}

/* (g_gl.mu held) the part's library, or VK_NULL_HANDLE; *slot: where it goes */
static VkPipeline part_find(unsigned kind, uint64_t key, int *slot)
{
    Part *t = g_gl.tab[kind];
    const uint64_t k = key ? key : 1;           /* (0 marks a free slot) */
    unsigned i = (unsigned)(k ^ (k >> 29)) & (PART_CAP - 1);
    for (unsigned n = 0; n < PART_CAP; n++, i = (i + 1) & (PART_CAP - 1)) {
        if (!t[i].key) {
            *slot = g_gl.n[kind] + 1 < PART_CAP * 3 / 4 ? (int)i : -1;
            return VK_NULL_HANDLE;
        }
        if (t[i].key == k) {
            *slot = (int)i;                     /* (its own) */
            return t[i].lib;
        }
    }
    *slot = -1;
    return VK_NULL_HANDLE;
}

/* The part (kind, key), from the table or built from ci_in; its stages'
 * modules made first by d->make (which: 0 the vertex part's, 1 the pixel
 * part's, into stages) when it is built. */
static HlePbResult part_get(unsigned kind, uint64_t key, const VkGraphicsPipelineCreateInfo *ci_in,
                            const HleGplDesc *d, unsigned which, VkPipelineShaderStageCreateInfo *stages,
                            bool cache_only, VkPipeline *out, double *built_ms)
{
    int slot;
    bool own = false;                           /* this thread builds it into the table */
    hle_mutex_lock(&g_gl.mu);
    for (;;) {
        VkPipeline p = part_find(kind, key, &slot);
        if (p == PART_BUILDING) {
            if (cache_only) {
                hle_mutex_unlock(&g_gl.mu);
                return HLE_PB_COMPILE;          /* (not waited for: a cache-only look) */
            }
            hle_cond_wait(&g_gl.cv, &g_gl.mu);
            continue;
        }
        if (p && p != PART_FAILED) {
            hle_mutex_unlock(&g_gl.mu);
            *out = p;
            return HLE_PB_OK;
        }
        if (!p && slot >= 0 && !cache_only) {
            g_gl.tab[kind][slot].key = key ? key : 1;   /* claimed: built here */
            g_gl.tab[kind][slot].lib = PART_BUILDING;
            g_gl.n[kind]++;
            own = true;
        }
        break;
    }
    hle_mutex_unlock(&g_gl.mu);
    if (!own && slot < 0) {
        return HLE_PB_FAILED;                   /* the table full: built whole */
    }
    VkPipeline p = VK_NULL_HANDLE;
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    HlePbResult mr = HLE_PB_OK;
    if (stages && d->make) {
        mr = d->make(d->make_ctx, which, stages);   /* (no SPIR-V: from the caches only, or it failed) */
    }
    if (mr == HLE_PB_OK) {
        VkGraphicsPipelineLibraryCreateInfoEXT li = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
            .pNext = ci_in->pNext, .flags = k_part_flag[kind] };
        VkGraphicsPipelineCreateInfo ci = *ci_in;
        ci.pNext = &li;
        ci.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
                    (cache_only ? VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT : 0);
        const int64_t t0 = g_get_monotonic_time();
        hle_precision_build_enter();
        vr = T.vkCreateGraphicsPipelines(g_hvk.dev, hle_pcache_get(), 1, &ci, NULL, &p);
        hle_precision_build_leave();
        *built_ms += (g_get_monotonic_time() - t0) / 1000.0;
        if (vr != VK_SUCCESS) {
            p = VK_NULL_HANDLE;
        } else if (!cache_only) {
            hle_pcache_compiled();
        }
    }
    if (own) {
        hle_mutex_lock(&g_gl.mu);
        g_gl.tab[kind][slot].lib = p ? p : PART_FAILED;
        hle_cond_broadcast(&g_gl.cv);
        hle_mutex_unlock(&g_gl.mu);
    } else if (p && cache_only) {
        /* from the caches: into the table if still free (else the one there) */
        int at;
        hle_mutex_lock(&g_gl.mu);
        const VkPipeline had = part_find(kind, key, &at);
        if (!had && at >= 0) {
            g_gl.tab[kind][at].key = key ? key : 1;
            g_gl.tab[kind][at].lib = p;
            g_gl.n[kind]++;
            own = true;
        }
        hle_mutex_unlock(&g_gl.mu);
        if (had && had != PART_BUILDING && had != PART_FAILED) {
            T.vkDestroyPipeline(g_hvk.dev, p, NULL);
            p = had;
            own = true;
        }
    }
    if (mr != HLE_PB_OK) {
        return mr;
    }
    if (vr == VK_PIPELINE_COMPILE_REQUIRED) {
        return HLE_PB_COMPILE;
    }
    if (!p) {
        return HLE_PB_FAILED;
    }
    *out = p;
    /* (another thread's failed build, or a cache-only one: kept only as long as
     * this link needs it, then let go) */
    return own ? HLE_PB_OK : HLE_PB_FAILED;
}

/* The render thread's builds: their pixel part made by a thread of its own
 * while the render thread makes the vertex part */
static struct {
    bool started;
    int busy;                   /* (one build at a time: another thread's goes without) */
    HleThread th;
    HleSem go, done;
    uint64_t key;
    const VkGraphicsPipelineCreateInfo *ci;
    const HleGplDesc *d;
    VkPipelineShaderStageCreateInfo *stages;
    VkPipeline out;
    double ms;
    HlePbResult r;
} g_side;

static void *side_main(void *opaque)
{
    (void)opaque;
    for (;;) {
        hle_sem_wait(&g_side.go);
        g_side.ms = 0;
        g_side.out = VK_NULL_HANDLE;
        g_side.r = part_get(PART_FS, g_side.key, g_side.ci, g_side.d, 1, g_side.stages, false, &g_side.out,
                            &g_side.ms);
        hle_sem_post(&g_side.done);
    }
    return NULL;
}

/* The libraries hold the part of that key */
static bool part_have(unsigned kind, uint64_t key, int *slot)
{
    hle_mutex_lock(&g_gl.mu);
    const VkPipeline p = part_find(kind, key, slot);
    hle_mutex_unlock(&g_gl.mu);
    return p != VK_NULL_HANDLE && p != PART_FAILED;     /* (one being built: waited for) */
}

/* The pixel part to the side thread if it is to be built: true when it took it */
static bool side_start(uint64_t key, const VkGraphicsPipelineCreateInfo *ci, const HleGplDesc *d,
                       VkPipelineShaderStageCreateInfo *stages)
{
    int slot;
    if (part_have(PART_FS, key, &slot) || slot < 0 ||
        __atomic_exchange_n(&g_side.busy, 1, __ATOMIC_ACQUIRE)) {
        return false;
    }
    if (!g_side.started) {
        hle_sem_init(&g_side.go, 0);
        hle_sem_init(&g_side.done, 0);
        hle_thread_create(&g_side.th, "hle.gplside", side_main, NULL, HLE_THREAD_DETACHED);
        g_side.started = true;
    }
    g_side.key = key;
    g_side.ci = ci;
    g_side.d = d;
    g_side.stages = stages;
    hle_sem_post(&g_side.go);
    return true;
}

HlePbResult hle_gpl_build(const HleGplDesc *d, bool cache_only, bool side, VkPipeline *out, double *built_ms)
{
    *out = VK_NULL_HANDLE;
    *built_ms = 0;
    VkPipelineRenderingCreateInfo ri = *d->ri, ri_pre = *d->ri;
    ri.pNext = NULL;
    ri_pre.pNext = d->pre_next;
    /* the stages, their modules filled in by d->make where a part is built */
    VkPipelineShaderStageCreateInfo pre_st[2] = { d->pre_stages[0] }, fs_st = *d->fs_stage;
    if (d->npre_stages > 1) {
        pre_st[1] = d->pre_stages[1];
    }
    /* the parts' state: each only what it is made of */
    const VkGraphicsPipelineCreateInfo vi_ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pVertexInputState = d->vis, .pInputAssemblyState = d->ia, .pDynamicState = d->dy };
    const VkGraphicsPipelineCreateInfo pre_ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri_pre,
        .stageCount = d->npre_stages, .pStages = pre_st,
        .pViewportState = d->vp, .pRasterizationState = d->rs, .pDynamicState = d->dy,
        .layout = d->layout };
    const VkGraphicsPipelineCreateInfo fs_ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
        .stageCount = 1, .pStages = &fs_st,
        .pDepthStencilState = d->ds, .pMultisampleState = d->ms, .pDynamicState = d->dy,
        .layout = d->layout };
    const VkGraphicsPipelineCreateInfo out_ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
        .pColorBlendState = d->cb, .pMultisampleState = d->ms, .pDynamicState = d->dy,
        .layout = d->layout };
    const VkGraphicsPipelineCreateInfo *const ci[PART_N] = { &vi_ci, &pre_ci, &fs_ci, &out_ci };
    VkPipelineShaderStageCreateInfo *const stages[PART_N] = { NULL, pre_st, &fs_st, NULL };
    const unsigned which[PART_N] = { 0, 0, 1, 0 };
    const uint64_t keys[PART_N] = { d->vi_key, d->pre_key, d->fs_key, d->out_key };
    VkPipeline libs[PART_N] = { 0 };
    HlePbResult r = HLE_PB_OK;
    bool loose[PART_N] = { false };
    if (d->have_only) {
        hle_mutex_lock(&g_gl.mu);
        for (unsigned k = PART_PRE; k <= PART_FS; k++) {
            int slot;
            const VkPipeline p = part_find(k, keys[k], &slot);
            if (!p || p == PART_BUILDING || p == PART_FAILED) {
                r = HLE_PB_COMPILE;
            }
        }
        hle_mutex_unlock(&g_gl.mu);
        if (r != HLE_PB_OK) {
            return r;
        }
    }
    const bool beside = side && !cache_only && !d->have_only && side_start(keys[PART_FS], ci[PART_FS], d, &fs_st);
    for (unsigned k = 0; k < PART_N && r == HLE_PB_OK; k++) {
        if (k == PART_FS && beside) {
            continue;
        }
        r = part_get(k, keys[k], ci[k], d, which[k], stages[k], cache_only, &libs[k], built_ms);
        if (r == HLE_PB_FAILED && libs[k]) {
            loose[k] = true;                    /* not kept (part_get) */
            r = HLE_PB_OK;
        }
    }
    if (beside) {                               /* (its state is on this stack: waited for whatever) */
        hle_sem_wait(&g_side.done);
        libs[PART_FS] = g_side.out;
        *built_ms += g_side.ms;
        if (g_side.r == HLE_PB_FAILED && g_side.out) {
            loose[PART_FS] = true;
        } else if (g_side.r != HLE_PB_OK && r == HLE_PB_OK) {
            r = g_side.r;
        }
        __atomic_store_n(&g_side.busy, 0, __ATOMIC_RELEASE);
    }
    if (r == HLE_PB_OK) {
        VkPipelineLibraryCreateInfoKHR lci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
            .libraryCount = PART_N, .pLibraries = libs };
        const VkGraphicsPipelineCreateInfo link = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &lci,
            .flags = cache_only ? VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT : 0,
            .layout = d->layout };
        hle_precision_build_enter();
        hle_precision_link_enter();
        const VkResult vr = T.vkCreateGraphicsPipelines(g_hvk.dev, hle_pcache_get(), 1, &link, NULL, out);
        hle_precision_link_leave();
        hle_precision_build_leave();
        r = vr == VK_SUCCESS ? HLE_PB_OK : vr == VK_PIPELINE_COMPILE_REQUIRED ? HLE_PB_COMPILE : HLE_PB_FAILED;
        if (r != HLE_PB_OK) {
            *out = VK_NULL_HANDLE;
        }
    }
    for (unsigned k = 0; k < PART_N; k++) {
        if (loose[k]) {
            T.vkDestroyPipeline(g_hvk.dev, libs[k], NULL);
        }
    }
    return r;
}

void hle_gpl_counts(unsigned *vi, unsigned *pre, unsigned *fs, unsigned *out)
{
    *vi = g_gl.n[PART_VI];
    *pre = g_gl.n[PART_PRE];
    *fs = g_gl.n[PART_FS];
    *out = g_gl.n[PART_OUT];
}
