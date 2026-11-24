/*
 * xbased HLE GPU — a driver crash in a pipeline build, kept from the next run.
 *
 * The crash handler (native/core/fault_win.c, nc_crash_hook) asks whether the
 * crashing thread was in the driver's build of a pipeline:
 *  - the pipeline itself (its key's hash) is not built again on that GPU and
 *    driver, its draws skipped: an effect missing rather than the game gone
 *    (AMD's driver for GCN, 22.40 on a Vega 3, crashed building one of OutRun
 *    2006's as a race loaded, every time, #329);
 *  - a crash in the link of one from its libraries' parts (gpl.c) turns the
 *    libraries off for that GPU and driver (whole pipelines, built
 *    asynchronously: AMD's Windows driver 26.11 on an RX 6600 crashed linking
 *    them in Halo 2, #325).
 * The verdicts' file is <base>/gpu_precision.txt, its lines
 * "<vendor:device:driver>/pipe:<key> off <why>" and "<...>/gpl off <why>".
 */
#ifndef HW_XBOX_HLE_VK_DRVCRASH_H
#define HW_XBOX_HLE_VK_DRVCRASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/* (vkdev.c, after the device) this GPU's id, its pipelines kept out read */
void hle_drvcrash_init(VkPhysicalDevice phys);
/* Around a driver call that builds a pipeline (link: from its libraries) */
void hle_drvcrash_build_enter(void);
void hle_drvcrash_build_leave(void);
void hle_drvcrash_link_enter(void);
void hle_drvcrash_link_leave(void);
/* The pipeline this thread builds (its key's hash, 0 none) */
void hle_drvcrash_pipe_set(uint64_t key);
/* It crashed this GPU and driver in an earlier run */
bool hle_drvcrash_pipe_blocked(uint64_t key);
/* The libraries crashed its link in an earlier run (why: the reason) */
bool hle_drvcrash_gpl_blocked(char *why, size_t wn);
/* The crash handler's */
void hle_drvcrash_crashed(void);

#endif
