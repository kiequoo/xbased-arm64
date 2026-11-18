/*
 * xbased HLE GPU — the GPU precision (precision.c): the NV2A's own arithmetic
 * in the shaders (exact: the transform unit's operations, the colour
 * weights, texel ties and depth words in doubles and 64-bit integers), or
 * plain float (fast: the stand-ins' forms for every draw, close to 1.0.94's).
 *
 * The setting (precision=auto|exact|fast) picks one; auto decides per GPU
 * and driver, in this order, each with its reason in the log:
 *   - AMD's proprietary driver on a GCN GPU (Polaris, Vega: the Windows
 *     driver branch AMD keeps for them since 2023): fast. Its shader
 *     compiler crashed building exact pixel shaders (issues #306, #307:
 *     an RX 580 and Vega graphics, driver 0x800117 on both); RDNA runs them.
 *     GCN is told from its subgroups (64 lanes only; RDNA has 32 too).
 *   - a driver that crashed building a shader in an earlier run (the crash
 *     handler's mark, this GPU and driver version): fast.
 *   - a host of 4 threads or fewer: fast. The emulation keeps several of
 *     its own busy, and the exact forms are built by threads beside them:
 *     on a 2-thread Celeron with Intel graphics (#309) an exact program took
 *     25 to 28 s to build, the game at 18 fps meanwhile (30 on 1.0.104).
 *   - measured: a run whose exact forms cost too much to build (one 5 s or
 *     more, or 20 s in all) or whose GPU stayed at its limit with them (busy
 *     95% of the time for 15 s, under 57 frames a second) marks this GPU
 *     and driver fast from the next run.
 *   - else exact.
 * The verdicts are kept in <base>/gpu_precision.txt, a line per GPU and
 * driver version; a new driver version decides again.
 */
#ifndef HW_XBOX_HLE_VK_PRECISION_H
#define HW_XBOX_HLE_VK_PRECISION_H

#include <stdbool.h>
#include <stdint.h>

#include "hw/xbox/hle/vk/vkdev.h"

enum { HLE_PREC_AUTO, HLE_PREC_EXACT, HLE_PREC_FAST };

/* The setting, before the device is made (nc_hle_set_precision). */
void hle_precision_set_option(int mode);
/* The decision for the device chosen (vkdev.c, after its creation). */
void hle_precision_init(VkPhysicalDevice phys);
/* Fast: plain float shaders for every draw (draw.c's keys, psh.c's code). */
bool hle_precision_fast(void);
/* Auto, exact for now: the GPU's busy time is measured (back.c's timestamps). */
bool hle_precision_measuring(void);
/* A command buffer's GPU time (ns), as it completes; a frame of the game's. */
void hle_precision_gpu_time(uint64_t ns);
void hle_precision_swap(void);
/* An exact form built (ms): auto's measure of what they cost this host. */
void hle_precision_exact_build(double ms);
/* Around a call into the driver that compiles (shader modules, pipelines):
 * a crash meanwhile is the driver's (hle_precision_crashed). */
void hle_precision_build_enter(void);
void hle_precision_build_leave(void);
/* The crash handler's: if the crashing thread was in a driver build, this
 * GPU and driver go fast from the next run (auto). */
void hle_precision_crashed(void);
/* The same around the link of a pipeline from its libraries' parts
 * (gpl.c): a driver crash there turns the libraries off for this GPU and
 * driver from the next run (AMD's Windows driver 26.11 on RDNA 2 crashed
 * linking them, any precision, #325); hle_precision_gpl_blocked: that
 * verdict, its reason in why. */
void hle_precision_link_enter(void);
void hle_precision_link_leave(void);
bool hle_precision_gpl_blocked(char *why, size_t wn);
/* The pipeline this thread builds (its key's hash, 0 none): a driver crash
 * in its build keeps it from being built again on this GPU and driver
 * (hle_precision_pipe_blocked from the next run: its draws are skipped).
 * AMD's driver for GCN GPUs (22.40 on a Vega 3) crashed building one of
 * OutRun 2006's as a race loaded, on fast precision too, at every try
 * (#329): a missing effect rather than the game gone. */
void hle_precision_pipe_set(uint64_t key);
bool hle_precision_pipe_blocked(uint64_t key);

#endif
