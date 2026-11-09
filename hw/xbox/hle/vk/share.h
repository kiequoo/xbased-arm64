/*
 * hakuX HLE GPU — the window's pictures handed to another process (Windows).
 *
 * The game's frame (native/app/win_frame.c) owns the one top-level window of
 * a game session and presents to it, with D3D12 and a DXGI swapchain; each
 * run of the emulator draws its final picture (the frame scaled, filtered,
 * the overlay over it) into images the frame made for it, as it would into
 * a swapchain's. The driver sees the frame's process presenting to a full
 * screen window of its own: independent flip and G-Sync / FreeSync as for
 * any game, and a run started again changes where the pictures come from,
 * not the window.
 *
 * As RetroArch presents Vulkan through DXGI (gfx/common/win_vulkan_dxgi.c):
 * the images are D3D12 committed resources and the fences D3D12 fences, both
 * shared (NT handles), imported by the run's Vulkan device as
 * VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT memory and binary
 * semaphores from VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT, their
 * values given with VkD3D12FenceSubmitInfoKHR. The handles are the frame's;
 * the run duplicates them into itself (DuplicateHandle), nothing by name.
 *
 * A run asks (its source's size, GPU, then gen), the frame makes the images
 * and the fence and answers (made = gen, or -gen and why). Per picture v into
 * image i: the run waits (CPU) for `freed` >= what i held last (the frame
 * copied it, or one after it), draws, its submit signals the `ready` fence
 * = v, and it writes latest; the frame waits (CPU) for `ready` >= v, copies,
 * presents, and once its copy is done writes `freed` = v. No GPU waits on the
 * other process's fences (as RetroArch's bridge, whose GPU wait always finds
 * the value reached, the CPU having waited first): a run gone mid-picture
 * can't hold the frame's queue, nor the frame a run's.
 */
#ifndef HLE_VK_SHARE_H
#define HLE_VK_SHARE_H

#include <stdint.h>

#define HLE_SHARE_MAGIC   0x58424652u  /* "XBFR" */
#define HLE_SHARE_VERSION 2u
#define HLE_SHARE_IMAGES  3u
#define HLE_SHARE_MICE    64           /* the mouse's events from the frame, a ring */
#define HLE_SHARE_FORMAT  87u          /* DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM's */

/* one run drawing for the frame */
typedef struct HleShareSrc {
    volatile int64_t gen;              /* the run's ask (0: none); written last */
    volatile int64_t order;            /* the share's count when it came: the newest shows */
    uint32_t pid;
    uint32_t w, h;                     /* its images */
    uint32_t vsync;                    /* 1 FIFO, 0 with tearing (VRR's below its range) */
    uint8_t luid[8];                   /* its GPU's (VkPhysicalDeviceIDProperties) */
    uint32_t luid_valid;
    /* the frame's answer: its handles (in the frame's process), then made */
    uint64_t res[HLE_SHARE_IMAGES];
    uint64_t ready;
    volatile int64_t made;             /* gen: made; -gen: couldn't (the block's why) */
    volatile int64_t latest;           /* (v << 2) | image: its newest picture */
    volatile int64_t shown;            /* the frame's: the newest it presented (v) */
    volatile int64_t freed;            /* the frame's: done with pictures up to v (copied, or let go) */
} HleShareSrc;

typedef struct HleShareMouse {
    uint32_t type;                     /* 1 move, 2 button down, 3 button up, 4 wheel */
    int32_t x, y;                      /* client pixels */
    int32_t button, wheel;             /* SDL_BUTTON_*, wheel steps (x120 -> 1) */
} HleShareMouse;

typedef struct HleShare {
    uint32_t magic, version;
    uint32_t frame_pid;                /* the handles' process */
    volatile uint32_t cw, ch;          /* the frame's client area, pixels */
    volatile int64_t orders;           /* sources so far */
    volatile int32_t cursor;           /* the run wants the pointer shown */
    volatile int32_t failed;           /* the frame can't present them: the runs' windows their own */
    char why[128];                     /* what failed (the frame's word), for the run's log */
    volatile int64_t mouse_head;       /* written by the frame */
    HleShareMouse mouse[HLE_SHARE_MICE];
    HleShareSrc src[2];
} HleShare;

/* the block's and the new-picture event's names for a frame's token */
#define HLE_SHARE_BLOCK_FMT  L"Local\\xbased-%hs-share"
#define HLE_SHARE_EVENT_FMT  L"Local\\xbased-%hs-new"
#define HLE_SHARE_FREED_FMT  L"Local\\xbased-%hs-freed%d"   /* source k's freed moved */

#endif
