/*
 * xbased HLE GPU — pipelines linked from libraries (VK_EXT_graphics_pipeline_library):
 * a pipeline's four parts (vertex input, the vertex stage with the
 * rasterizer's state, the pixel stage with the depth test's, the colour
 * output) each built once and kept by what it is made of, a new pipeline
 * linked from them without optimizing across the stages (a link is under a
 * millisecond). A vertex program drawn with ten pixel stages was built ten
 * times whole; an exact one (shader/xflib.h) took the driver up to a second
 * each, its stand-in tens of milliseconds.
 */
#ifndef HW_XBOX_HLE_VK_GPL_H
#define HW_XBOX_HLE_VK_GPL_H

#include <stdbool.h>
#include <stdint.h>
#include <volk.h>
#include "hw/xbox/hle/vk/pipeasync.h"

/* A pipeline's state as draw.c fills it, and the keys of its parts (what
 * each is made of, hashed by the caller): the pre-rasterization part's
 * stages are the vertex stage and a geometry stage (stage[1], or none).
 * Their modules may be left VK_NULL_HANDLE for make (make_ctx, which, stages)
 * to fill in when that part is built: which 0, the vertex part's (stages[0],
 * and [1] with a geometry stage); 1, the pixel part's (stages[0]). Any thread
 * may call it, the pixel part's beside the vertex part's (no shared state
 * between the two). HLE_PB_COMPILE: no SPIR-V in the caches (cache_only). */
typedef struct HleGplDesc {
    uint64_t vi_key, pre_key, fs_key, out_key;
    const VkPipelineShaderStageCreateInfo *pre_stages;
    uint32_t npre_stages;
    const VkPipelineShaderStageCreateInfo *fs_stage;
    const VkPipelineVertexInputStateCreateInfo *vis;
    const VkPipelineInputAssemblyStateCreateInfo *ia;
    const VkPipelineViewportStateCreateInfo *vp;
    const VkPipelineRasterizationStateCreateInfo *rs;
    const VkPipelineMultisampleStateCreateInfo *ms;
    const VkPipelineDepthStencilStateCreateInfo *ds;
    const VkPipelineColorBlendStateCreateInfo *cb;
    const VkPipelineDynamicStateCreateInfo *dy;
    const VkPipelineRenderingCreateInfo *ri;      /* its pNext: none */
    const void *pre_next;           /* the pre-rasterization part's own chain (discard rectangles), or NULL */
    VkPipelineLayout layout;
    HlePbResult (*make)(void *ctx, unsigned which, VkPipelineShaderStageCreateInfo *stages);
    void *make_ctx;
    bool have_only;                 /* the two shader parts as the tables hold them, built:
                                       HLE_PB_COMPILE when either is not (vk/draw.c's
                                       interpreter pipelines, linked at a draw) */
} HleGplDesc;

/* The device links libraries (caps.gpl with dynamic vertex input and extended
 * dynamic state) and XBASED_HLE_GPL is not 0. */
bool hle_gpl_on(void);
/* The pipeline from its parts, those missing built (cache_only: from the
 * caches only, HLE_PB_COMPILE when one would compile); *built_ms: the time
 * spent building parts. Any thread; side (the render thread's builds only,
 * one at a time): a new pixel part built by a thread of its own meanwhile. */
HlePbResult hle_gpl_build(const HleGplDesc *d, bool cache_only, bool side, VkPipeline *out,
                          double *built_ms);
/* Parts kept, for the log */
void hle_gpl_counts(unsigned *vi, unsigned *pre, unsigned *fs, unsigned *out);

#endif
