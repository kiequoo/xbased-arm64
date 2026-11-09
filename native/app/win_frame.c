/*
 * xbased — the game's frame (win_frame.h): Windows, then Linux (X11).
 */
#include "native/app/win_frame.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include "hw/xbox/hle/vk/share.h"

#define WM_FRAME_TOGGLE (WM_APP + 1)
#define WM_FRAME_CLOSE  (WM_APP + 2)
#define FRAME_TICK_MS   50
#define FRAME_LINGER_MS 3000    /* no run's window in it this long: the session's over */
#define FRAME_WAIT_MS   10000   /* the first run's window this long at most */

/* ------------------------------------------------------------ the frame's process */

static HWND g_fw;
static bool g_fs, g_fs60, g_mode_set, g_had_child;
static RECT g_wrect;                /* windowed, the place to go back to */
static char g_dev[CCHDEVICENAME];   /* the monitor whose mode was set */
static ULONGLONG g_t0, g_gone_at;

static HWND top_child(void)
{
    return GetWindow(g_fw, GW_CHILD);
}

static void fit_children(void)
{
    RECT rc;
    GetClientRect(g_fw, &rc);
    for (HWND c = top_child(); c; c = GetWindow(c, GW_HWNDNEXT)) {
        RECT cr;
        GetWindowRect(c, &cr);
        if (cr.right - cr.left <= 1) {
            continue;   /* a run's window kept out of sight (the frame presents its pictures) */
        }
        SetWindowPos(c, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* fsrate=60: the monitor's highest mode at its size that's a whole number
 * of 60 Hz (59 too: 59.94), no faster than now; the current one if it's
 * one already (win_main.c fs_mode's, in Win32 terms) */
static void mode_set(const char *dev)
{
    DEVMODEA cur;
    memset(&cur, 0, sizeof(cur));
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsA(dev, ENUM_CURRENT_SETTINGS, &cur)) {
        return;
    }
    const DWORD f0 = cur.dmDisplayFrequency;
    if (f0 % 60 == 0 || (f0 + 1) % 60 == 0) {
        return;
    }
    DEVMODEA best, dm;
    bool have = false;
    for (DWORD i = 0;; i++) {
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsA(dev, i, &dm)) {
            break;
        }
        const DWORD f = dm.dmDisplayFrequency;
        if (dm.dmPelsWidth == cur.dmPelsWidth && dm.dmPelsHeight == cur.dmPelsHeight &&
            dm.dmBitsPerPel == cur.dmBitsPerPel && !(dm.dmDisplayFlags & DM_INTERLACED) && f <= f0 && f >= 59 &&
            f <= 241 && (f % 60 == 0 || (f + 1) % 60 == 0) && (!have || f > best.dmDisplayFrequency)) {
            best = dm;
            have = true;
        }
    }
    if (have) {
        best.dmFields &= DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;
        if (ChangeDisplaySettingsExA(dev, &best, NULL, CDS_FULLSCREEN, NULL) == DISP_CHANGE_SUCCESSFUL) {
            g_mode_set = true;
            snprintf(g_dev, sizeof(g_dev), "%s", dev);
        }
    }
}

static void mode_restore(void)
{
    if (g_mode_set) {
        ChangeDisplaySettingsExA(g_dev, NULL, NULL, 0, NULL);
        g_mode_set = false;
    }
}

/* The taskbar told the frame's a full screen window: it stays under it.
 * Left to its own look, the taskbar came on top for a moment after the
 * frame took the foreground (the run's window coming into it), over the
 * game on screens where it doesn't hide itself. */
static void mark_fullscreen(bool on)
{
    static ITaskbarList2 *tb;
    static bool tried;
    if (!tried) {
        tried = true;
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (FAILED(CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList2, (void **)&tb))) {
            tb = NULL;
        } else if (FAILED(ITaskbarList2_HrInit(tb))) {
            ITaskbarList2_Release(tb);
            tb = NULL;
        }
    }
    if (tb) {
        ITaskbarList2_MarkFullscreenWindow(tb, g_fw, on ? TRUE : FALSE);
    }
}

static void set_fullscreen(bool on)
{
    if (on == g_fs) {
        return;
    }
    mark_fullscreen(on);
    if (on) {
        GetWindowRect(g_fw, &g_wrect);
        MONITORINFOEXA mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        GetMonitorInfoA(MonitorFromWindow(g_fw, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi);
        if (g_fs60) {
            mode_set(mi.szDevice);
            GetMonitorInfoA(MonitorFromWindow(g_fw, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi);
        }
        const LONG_PTR vis = IsWindowVisible(g_fw) ? WS_VISIBLE : 0;   /* not shown by this, if it wasn't yet */
        SetWindowLongPtrA(g_fw, GWL_STYLE, WS_POPUP | vis | WS_CLIPCHILDREN);
        const RECT r = mi.rcMonitor;
        SetWindowPos(g_fw, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
    } else {
        mode_restore();
        SetWindowLongPtrA(g_fw, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN);
        /* its place back, the title bar on the screen whatever it was */
        RECT r = g_wrect;
        MONITORINFO wi = {sizeof(wi)};
        GetMonitorInfoA(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &wi);
        if (r.top < wi.rcWork.top) {
            OffsetRect(&r, 0, wi.rcWork.top - r.top);
        }
        SetWindowPos(g_fw, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED);
    }
    g_fs = on;
    fit_children();
}


/* ------------------------------------------------------------ the frame's presenter */

/* The runs' pictures (hw/xbox/hle/vk/share.h) presented to the frame's own
 * window with D3D12 and a DXGI flip swapchain, on a thread of its own: the
 * frame makes each run's images and fences (shared), waits on its queue for
 * the newest picture a run has drawn, copies it into the swapchain's buffer
 * and presents it, at sync interval 1 (V-Sync), or 0 with tearing. The
 * process that owns the window presents to it, as any D3D12 game does:
 * independent flip, G-Sync and FreeSync on every GPU. As RetroArch's
 * Vulkan-through-DXGI presenter (gfx/common/win_dxgi_present.c): two
 * buffers, a frame latency of one (its waitable object, not waited at
 * interval 0 with tearing), FLIP_DISCARD, ALLOW_TEARING when there is. */

static HANDLE g_map, g_ev;
static HleShare *g_sh;
static volatile LONG g_presenting;  /* the presenter draws the window: no GDI black over it */

#define FP_BUFFERS 2

static const GUID fp_IID_ID3D12Device = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
static const GUID fp_IID_ID3D12CommandQueue = {0x0ec870a6, 0x5d7e, 0x4c22, {0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed}};
static const GUID fp_IID_ID3D12Fence = {0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}};
static const GUID fp_IID_ID3D12Resource = {0x696442be, 0xa72e, 0x4059, {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}};
static const GUID fp_IID_ID3D12CommandAllocator = {0x6102dee4, 0xaf59, 0x4b09, {0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24}};
static const GUID fp_IID_ID3D12GraphicsCommandList = {0x5b160d0f, 0xac1b, 0x4185, {0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55}};
static const GUID fp_IID_IDXGIFactory2 = {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
static const GUID fp_IID_IDXGIFactory5 = {0x7632e1f5, 0xee65, 0x4dca, {0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d}};
static const GUID fp_IID_IDXGISwapChain3 = {0x94d99bdb, 0xf1f8, 0x4ab0, {0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1}};

#define FP_DROP(p)                                  \
    do {                                            \
        if (p) {                                    \
            (p)->lpVtbl->Release(p);                \
            (p) = NULL;                             \
        }                                           \
    } while (0)

/* one run's images and fences */
typedef struct FpSrc {
    int64_t gen;                        /* the ask they answer (0: none) */
    uint32_t w, h;
    ID3D12Resource *res[HLE_SHARE_IMAGES];
    HANDLE res_h[HLE_SHARE_IMAGES];
    ID3D12Fence *ready;                 /* the run's: its pictures drawn (v) */
    HANDLE ready_h;
    HANDLE freed_ev;                    /* its freed moved (vk/share.h) */
    uint64_t taken;                     /* the newest picture taken (presented, or let go) */
} FpSrc;

static struct {
    bool failed;
    IDXGIFactory2 *factory;
    BOOL tearing;
    ID3D12Device *dev;
    ID3D12CommandQueue *q;
    ID3D12Fence *fence;                 /* the frame's own: its copies done */
    HANDLE fence_ev, ready_ev;
    uint64_t fence_v;
    IDXGISwapChain3 *sc;
    HANDLE latency;
    uint32_t sw, sh;
    ID3D12Resource *back[FP_BUFFERS];
    ID3D12CommandAllocator *alloc[FP_BUFFERS];
    ID3D12GraphicsCommandList *list[FP_BUFFERS];
    uint64_t used[FP_BUFFERS];          /* the frame's fence once its last copy is done */
    FpSrc s[2];
    int present_bad;
} P;

/* the frame can't present the pictures: the runs draw in their own
 * windows, told why (their log) */
static void fp_fail(const char *what, HRESULT hr)
{
    P.failed = true;
    if (g_sh) {
        snprintf(g_sh->why, sizeof(g_sh->why), "%s (%08lx)", what, (unsigned long)hr);
        InterlockedExchange((volatile LONG *)&g_sh->failed, 1);
    }
    InterlockedExchange(&g_presenting, 0);
}

/* the frame's copies done up to v */
static bool fp_wait(uint64_t v)
{
    if (!v || ID3D12Fence_GetCompletedValue(P.fence) >= v) {
        return true;
    }
    if (FAILED(ID3D12Fence_SetEventOnCompletion(P.fence, v, P.fence_ev))) {
        return false;
    }
    return WaitForSingleObject(P.fence_ev, 2000) == WAIT_OBJECT_0;
}

/* D3D12 on the runs' GPU (their device's LUID), a queue, the frame's fence */
static bool fp_device(const uint8_t luid[8])
{
    typedef HRESULT(WINAPI * create_factory_t)(REFIID, void **);
    HMODULE dx = LoadLibraryA("dxgi.dll"), d3 = LoadLibraryA("d3d12.dll");
    create_factory_t create_factory = dx ? (create_factory_t)(void *)GetProcAddress(dx, "CreateDXGIFactory1") : NULL;
    PFN_D3D12_CREATE_DEVICE create_device = d3 ? (PFN_D3D12_CREATE_DEVICE)(void *)GetProcAddress(d3, "D3D12CreateDevice") : NULL;
    if (!create_factory || !create_device) {
        fp_fail("no D3D12 or DXGI", E_NOINTERFACE);
        return false;
    }
    HRESULT hr = create_factory(&fp_IID_IDXGIFactory2, (void **)&P.factory);
    if (FAILED(hr)) {
        fp_fail("no DXGI factory", hr);
        return false;
    }
    IDXGIFactory5 *f5 = NULL;
    P.tearing = FALSE;
    if (SUCCEEDED(IDXGIFactory2_QueryInterface(P.factory, &fp_IID_IDXGIFactory5, (void **)&f5))) {
        if (FAILED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &P.tearing,
                                                     sizeof(P.tearing)))) {
            P.tearing = FALSE;
        }
        FP_DROP(f5);
    }
    IDXGIAdapter1 *ad = NULL;
    for (UINT i = 0; IDXGIFactory2_EnumAdapters1(P.factory, i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &d)) && !memcmp(&d.AdapterLuid, luid, 8)) {
            break;
        }
        FP_DROP(ad);
    }
    if (!ad) {
        fp_fail("no DXGI adapter for the runs' GPU", DXGI_ERROR_NOT_FOUND);
        return false;
    }
    hr = create_device((IUnknown *)ad, D3D_FEATURE_LEVEL_11_0, &fp_IID_ID3D12Device, (void **)&P.dev);
    FP_DROP(ad);
    if (FAILED(hr)) {
        fp_fail("no D3D12 device", hr);
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL};
    hr = ID3D12Device_CreateCommandQueue(P.dev, &qd, &fp_IID_ID3D12CommandQueue, (void **)&P.q);
    if (SUCCEEDED(hr)) {
        hr = ID3D12Device_CreateFence(P.dev, 0, D3D12_FENCE_FLAG_NONE, &fp_IID_ID3D12Fence, (void **)&P.fence);
    }
    for (unsigned i = 0; i < FP_BUFFERS && SUCCEEDED(hr); i++) {
        hr = ID3D12Device_CreateCommandAllocator(P.dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &fp_IID_ID3D12CommandAllocator,
                                                 (void **)&P.alloc[i]);
        if (SUCCEEDED(hr)) {
            hr = ID3D12Device_CreateCommandList(P.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, P.alloc[i], NULL,
                                                &fp_IID_ID3D12GraphicsCommandList, (void **)&P.list[i]);
        }
        if (SUCCEEDED(hr)) {
            hr = ID3D12GraphicsCommandList_Close(P.list[i]);
        }
    }
    P.fence_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    P.ready_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (FAILED(hr) || !P.fence_ev || !P.ready_ev) {
        fp_fail("no D3D12 queue, fence or command lists", hr);
        return false;
    }
    return true;
}

static void fp_drop_buffers(void)
{
    for (unsigned i = 0; i < FP_BUFFERS; i++) {
        FP_DROP(P.back[i]);
        P.used[i] = 0;
    }
}

/* the window's swapchain at its size */
static HRESULT fp_swapchain(uint32_t w, uint32_t h)
{
    if (P.sc && P.sw == w && P.sh == h) {
        return S_OK;
    }
    const UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                       (P.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    HRESULT hr;
    if (P.sc) {
        fp_wait(P.fence_v);     /* nothing of the frame's on the old buffers */
        fp_drop_buffers();
        hr = IDXGISwapChain3_ResizeBuffers(P.sc, FP_BUFFERS, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, flags);
    } else {
        DXGI_SWAP_CHAIN_DESC1 d = {.Width = w, .Height = h, .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
                                   .SampleDesc = {1, 0}, .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
                                   .BufferCount = FP_BUFFERS, .Scaling = DXGI_SCALING_STRETCH,
                                   .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD, .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
                                   .Flags = flags};
        IDXGISwapChain1 *sc1 = NULL;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(P.factory, (IUnknown *)P.q, g_fw, &d, NULL, NULL, &sc1);
        if (SUCCEEDED(hr)) {
            hr = IDXGISwapChain1_QueryInterface(sc1, &fp_IID_IDXGISwapChain3, (void **)&P.sc);
            FP_DROP(sc1);
        }
        if (SUCCEEDED(hr)) {
            hr = IDXGISwapChain3_SetMaximumFrameLatency(P.sc, 1);
        }
        if (SUCCEEDED(hr)) {
            P.latency = IDXGISwapChain3_GetFrameLatencyWaitableObject(P.sc);
            /* the window's full screen stays the frame's own (no Alt+Enter of DXGI's) */
            IDXGIFactory2_MakeWindowAssociation(P.factory, g_fw, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        }
    }
    for (unsigned i = 0; i < FP_BUFFERS && SUCCEEDED(hr); i++) {
        hr = IDXGISwapChain3_GetBuffer(P.sc, i, &fp_IID_ID3D12Resource, (void **)&P.back[i]);
    }
    if (SUCCEEDED(hr)) {
        P.sw = w;
        P.sh = h;
    }
    return hr;
}

static void fp_drop_source(FpSrc *s)
{
    fp_wait(P.fence_v);         /* the frame's copies from them done */
    for (unsigned i = 0; i < HLE_SHARE_IMAGES; i++) {
        FP_DROP(s->res[i]);
        if (s->res_h[i]) {
            CloseHandle(s->res_h[i]);
        }
        s->res_h[i] = NULL;
    }
    FP_DROP(s->ready);
    if (s->ready_h) {
        CloseHandle(s->ready_h);
    }
    s->ready_h = NULL;
    s->gen = 0;
    s->taken = 0;
}

/* source k's ask answered: its images and fences, shared, their handles
 * written (the run duplicates them into itself) */
static void fp_make(int k)
{
    HleShareSrc *src = &g_sh->src[k];
    FpSrc *s = &P.s[k];
    const int64_t gen = src->gen;
    fp_drop_source(s);
    const uint32_t w = src->w, h = src->h;
    D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_DEFAULT, .CreationNodeMask = 1, .VisibleNodeMask = 1};
    D3D12_RESOURCE_DESC rd = {.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = w, .Height = h,
                              .DepthOrArraySize = 1, .MipLevels = 1, .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
                              .SampleDesc = {1, 0}, .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
                              .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    HRESULT hr = w && h ? S_OK : E_INVALIDARG;
    for (unsigned i = 0; i < HLE_SHARE_IMAGES && SUCCEEDED(hr); i++) {
        hr = ID3D12Device_CreateCommittedResource(P.dev, &heap, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                  NULL, &fp_IID_ID3D12Resource, (void **)&s->res[i]);
        if (SUCCEEDED(hr)) {
            hr = ID3D12Device_CreateSharedHandle(P.dev, (ID3D12DeviceChild *)s->res[i], NULL, GENERIC_ALL, NULL,
                                                 &s->res_h[i]);
        }
    }
    if (SUCCEEDED(hr)) {
        hr = ID3D12Device_CreateFence(P.dev, 0, D3D12_FENCE_FLAG_SHARED, &fp_IID_ID3D12Fence, (void **)&s->ready);
    }
    if (SUCCEEDED(hr)) {
        hr = ID3D12Device_CreateSharedHandle(P.dev, (ID3D12DeviceChild *)s->ready, NULL, GENERIC_ALL, NULL, &s->ready_h);
    }
    if (FAILED(hr)) {
        fp_drop_source(s);
        snprintf(g_sh->why, sizeof(g_sh->why), "the run's images couldn't be made at %ux%u (%08lx)", w, h,
                 (unsigned long)hr);
        InterlockedExchange64((volatile LONG64 *)&src->made, -gen);
        return;
    }
    for (unsigned i = 0; i < HLE_SHARE_IMAGES; i++) {
        src->res[i] = (uint64_t)(uintptr_t)s->res_h[i];
    }
    src->ready = (uint64_t)(uintptr_t)s->ready_h;
    src->freed = 0;
    s->w = w;
    s->h = h;
    s->gen = gen;
    InterlockedExchange64((volatile LONG64 *)&src->made, gen);    /* last: the run takes them from now on */
}

/* the frame done with source k's pictures up to v: its images free again */
static void fp_freed(int k, uint64_t v)
{
    InterlockedExchange64((volatile LONG64 *)&g_sh->src[k].freed, (LONG64)v);
    SetEvent(P.s[k].freed_ev);
}

/* picture v of source k let go unseen (the window minimized, or another
 * run's shown): nothing of the frame's reads it */
static void fp_let_go(int k, uint64_t v)
{
    P.s[k].taken = v;
    fp_freed(k, v);
}

/* the run's drawing of picture v done (its fence, waited on the CPU: the
 * frame's queue never waits on another process's) */
static bool fp_drawn(FpSrc *s, uint64_t v, DWORD ms)
{
    const UINT64 at = ID3D12Fence_GetCompletedValue(s->ready);
    if (at == UINT64_MAX) {
        return false;   /* the run's device gone */
    }
    if (at >= v) {
        return true;
    }
    if (FAILED(ID3D12Fence_SetEventOnCompletion(s->ready, v, P.ready_ev))) {
        return false;
    }
    return WaitForSingleObject(P.ready_ev, ms) == WAIT_OBJECT_0;
}

static void fp_transition(D3D12_RESOURCE_BARRIER *b, ID3D12Resource *r, D3D12_RESOURCE_STATES from,
                          D3D12_RESOURCE_STATES to)
{
    memset(b, 0, sizeof(*b));
    b->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b->Transition.pResource = r;
    b->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b->Transition.StateBefore = from;
    b->Transition.StateAfter = to;
}

/* picture v (image i) of source k to the window: 1 shown, 0 let go (the
 * window minimized), 2 not drawn yet, -1 it couldn't be (why) */
static int fp_present(int k, uint64_t v, unsigned i)
{
    FpSrc *s = &P.s[k];
    RECT rc;
    GetClientRect(g_fw, &rc);
    const uint32_t w = (uint32_t)(rc.right - rc.left), h = (uint32_t)(rc.bottom - rc.top);
    if (IsIconic(g_fw) || !w || !h) {
        fp_let_go(k, v);
        return 0;
    }
    if (!fp_drawn(s, v, 250)) {
        return 2;       /* not drawn yet: again in a moment */
    }
    HRESULT hr = fp_swapchain(w, h);
    if (FAILED(hr)) {
        fp_let_go(k, v);
        snprintf(g_sh->why, sizeof(g_sh->why), "the window's swapchain (%08lx)", (unsigned long)hr);
        return -1;
    }
    const bool vsync = g_sh->src[k].vsync != 0;
    if ((vsync || !P.tearing) && P.latency) {
        WaitForSingleObject(P.latency, 100);    /* a frame queued at most */
    }
    const UINT b = IDXGISwapChain3_GetCurrentBackBufferIndex(P.sc) % FP_BUFFERS;
    fp_wait(P.used[b]);
    ID3D12CommandAllocator *al = P.alloc[b];
    ID3D12GraphicsCommandList *cl = P.list[b];
    ID3D12CommandAllocator_Reset(al);
    ID3D12GraphicsCommandList_Reset(cl, al, NULL);
    D3D12_RESOURCE_BARRIER bar[2];
    fp_transition(&bar[0], s->res[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    fp_transition(&bar[1], P.back[b], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 2, bar);
    if (s->w == P.sw && s->h == P.sh) {
        ID3D12GraphicsCommandList_CopyResource(cl, P.back[b], s->res[i]);
    } else {
        /* the window's size changing, before the run's next pictures: what fits */
        D3D12_TEXTURE_COPY_LOCATION dst = {.pResource = P.back[b], .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION srcl = {.pResource = s->res[i], .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_BOX box = {0, 0, 0, s->w < P.sw ? s->w : P.sw, s->h < P.sh ? s->h : P.sh, 1};
        ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &srcl, &box);
    }
    fp_transition(&bar[0], s->res[i], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    fp_transition(&bar[1], P.back[b], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 2, bar);
    hr = ID3D12GraphicsCommandList_Close(cl);
    if (FAILED(hr)) {
        fp_let_go(k, v);
        snprintf(g_sh->why, sizeof(g_sh->why), "the copy's command list (%08lx)", (unsigned long)hr);
        return -1;
    }
    ID3D12CommandList *lists[1] = {(ID3D12CommandList *)cl};
    ID3D12CommandQueue_ExecuteCommandLists(P.q, 1, lists);
    ID3D12CommandQueue_Signal(P.q, P.fence, ++P.fence_v);
    P.used[b] = P.fence_v;
    s->taken = v;
    hr = IDXGISwapChain3_Present(P.sc, vsync ? 1 : 0, !vsync && P.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    fp_wait(P.used[b]);     /* the copy done (a moment): the run's image free again */
    fp_freed(k, v);
    if (FAILED(hr)) {
        snprintf(g_sh->why, sizeof(g_sh->why), "present (%08lx)", (unsigned long)hr);
        return -1;
    }
    g_sh->src[k].shown = (int64_t)v;
    if (!g_presenting) {
        InterlockedExchange(&g_presenting, 1);
    }
    return 1;
}

static DWORD WINAPI fp_thread(void *arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    bool pending = false;
    for (;;) {
        WaitForSingleObject(g_ev, pending ? 1 : 50);
        pending = false;
        /* the runs' asks: their images made (D3D12 itself at the first) */
        for (int k = 0; k < 2; k++) {
            HleShareSrc *src = &g_sh->src[k];
            const int64_t gen = src->gen;
            if (gen && gen != P.s[k].gen && src->made != gen && src->made != -gen) {
                if (!P.dev && (!src->luid_valid || !fp_device(src->luid))) {
                    if (!P.failed) {
                        fp_fail("no D3D12 device for the runs' GPU", E_FAIL);
                    }
                    return 0;
                }
                fp_make(k);
            }
        }
        /* the newest run with a picture shows; the other's pictures let go */
        int best = -1;
        for (int k = 0; k < 2; k++) {
            const HleShareSrc *src = &g_sh->src[k];
            if (P.s[k].gen && src->made == P.s[k].gen && src->gen == P.s[k].gen && src->latest &&
                (best < 0 || src->order > g_sh->src[best].order)) {
                best = k;
            }
        }
        for (int k = 0; k < 2; k++) {
            const HleShareSrc *src = &g_sh->src[k];
            if (!P.s[k].gen || src->made != P.s[k].gen || src->gen != P.s[k].gen) {
                continue;
            }
            const int64_t latest = src->latest;
            const uint64_t v = (uint64_t)latest >> 2;
            if (!v || v <= P.s[k].taken) {
                continue;
            }
            if (k != best) {
                fp_let_go(k, v);
                continue;
            }
            const int r = fp_present(k, v, (unsigned)(latest & 3));
            if (r == 2) {
                pending = true;
            } else if (r > 0) {
                P.present_bad = 0;
            } else if (r < 0 && ++P.present_bad >= 10) {
                char why[sizeof(g_sh->why)];
                snprintf(why, sizeof(why), "%s", g_sh->why[0] ? g_sh->why : "the window couldn't be presented to");
                fp_fail(why, E_FAIL);
                return 0;
            }
        }
    }
}

/* the frame's process fell: the runs told why (their log), not left
 * drawing for a window that's gone */
static LONG WINAPI fp_crash(EXCEPTION_POINTERS *ep)
{
    if (g_sh && ep && ep->ExceptionRecord) {
        HMODULE m = NULL;
        char name[MAX_PATH] = "?";
        void *at = ep->ExceptionRecord->ExceptionAddress;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)at, &m)) {
            GetModuleFileNameA(m, name, sizeof(name));
        }
        const char *base = strrchr(name, '\\');
        snprintf(g_sh->why, sizeof(g_sh->why), "the frame's process crashed: %08lx at %s+%llx",
                 (unsigned long)ep->ExceptionRecord->ExceptionCode, base ? base + 1 : name,
                 (unsigned long long)((uintptr_t)at - (uintptr_t)m));
        InterlockedExchange((volatile LONG *)&g_sh->failed, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void fp_start(const char *token)
{
    wchar_t n[128];
    swprintf(n, 128, HLE_SHARE_BLOCK_FMT, token);
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(HleShare), n);
    g_sh = g_map ? (HleShare *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HleShare)) : NULL;
    swprintf(n, 128, HLE_SHARE_EVENT_FMT, token);
    g_ev = CreateEventW(NULL, FALSE, FALSE, n);
    if (!g_sh || !g_ev) {
        return;
    }
    memset(g_sh, 0, sizeof(*g_sh));
    RECT rc;
    GetClientRect(g_fw, &rc);
    g_sh->cw = (uint32_t)(rc.right - rc.left);
    g_sh->ch = (uint32_t)(rc.bottom - rc.top);
    g_sh->frame_pid = GetCurrentProcessId();
    for (int k = 0; k < 2; k++) {
        swprintf(n, 128, HLE_SHARE_FREED_FMT, token, k);
        P.s[k].freed_ev = CreateEventW(NULL, FALSE, FALSE, n);
    }
    g_sh->version = HLE_SHARE_VERSION;
    g_sh->magic = HLE_SHARE_MAGIC;      /* last: the runs open it from now on */
    SetUnhandledExceptionFilter(fp_crash);
    CloseHandle(CreateThread(NULL, 0, fp_thread, NULL, 0, NULL));
}

static void frame_mouse(uint32_t type, LPARAM l, int button, int wheel)
{
    if (!g_sh) {
        return;
    }
    POINT pt = {(short)LOWORD(l), (short)HIWORD(l)};
    if (type == 4) {
        ScreenToClient(g_fw, &pt);  /* the wheel's is the screen's */
    }
    const int64_t at = g_sh->mouse_head;
    HleShareMouse *m = &g_sh->mouse[at % HLE_SHARE_MICE];
    m->type = type;
    m->x = pt.x;
    m->y = pt.y;
    m->button = button;
    m->wheel = wheel;
    InterlockedExchange64((volatile LONG64 *)&g_sh->mouse_head, at + 1);
}

/* The session's over: the dashboard the game came from shown and in front
 * before the frame goes (this process has the foreground, it may hand it
 * on; the game's couldn't always). The frame gone first, the desktop and the
 * taskbar showed until the dashboard was back. */
static void hand_back(void)
{
    const char *e = getenv("XBASED_DASH_HWND");
    HWND d = e && e[0] ? (HWND)(uintptr_t)strtoull(e, NULL, 16) : NULL;
    if (d && IsWindow(d)) {
        ShowWindow(d, SW_SHOW);
        SetForegroundWindow(d);
    }
}

static LRESULT CALLBACK frame_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND: {
        if (g_presenting) {
            return 1;   /* the presenter's pictures: no GDI over them */
        }
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_SIZE:
        fit_children();
        if (g_sh && LOWORD(l) && HIWORD(l)) {
            g_sh->cw = LOWORD(l);   /* the runs draw at this size from their next picture */
            g_sh->ch = HIWORD(l);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && g_presenting) {
            SetCursor(g_sh && g_sh->cursor ? LoadCursorW(NULL, (LPCWSTR)IDC_ARROW) : NULL);
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        frame_mouse(1, l, 0, 0);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        frame_mouse(2, l, m == WM_LBUTTONDOWN ? 1 : m == WM_MBUTTONDOWN ? 2 : 3, 0);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        frame_mouse(3, l, m == WM_LBUTTONUP ? 1 : m == WM_MBUTTONUP ? 2 : 3, 0);
        return 0;
    case WM_MOUSEWHEEL:
        frame_mouse(4, l, 0, GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA);
        return 0;
    case WM_SYSCOMMAND:
        /* the system asks the window in front before starting the screen
         * saver or turning the panel off: never while a game's on (a pad
         * doesn't count as input), as SDL's own windows answer */
        if ((w & 0xFFF0) == SC_SCREENSAVE || (w & 0xFFF0) == SC_MONITORPOWER) {
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE && top_child()) {
            SetFocus(top_child());     /* the keyboard to the run's window */
            return 0;
        }
        break;
    case WM_SETFOCUS:
        if (top_child()) {
            SetFocus(top_child());
            return 0;
        }
        break;
    case WM_CLOSE:
        /* the window's close button: the run's own quit (its window's) */
        if (top_child()) {
            for (HWND c = top_child(); c; c = GetWindow(c, GW_HWNDNEXT)) {
                PostMessageA(c, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)l;
        if (!g_fs) {
            SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_FRAME_TOGGLE:
        set_fullscreen(!g_fs);
        return 0;
    case WM_FRAME_CLOSE:
        hand_back();
        ShowWindow(h, SW_HIDE);
        DestroyWindow(h);
        return 0;
    case WM_TIMER: {
        /* a run's window in it, or none: gone a moment between two runs
         * never (the next one is in before this one goes), so none for a
         * while is the session's end */
        const ULONGLONG now = GetTickCount64();
        if (top_child()) {
            g_had_child = true;
            g_gone_at = 0;
        } else if (g_had_child) {
            if (!g_gone_at) {
                g_gone_at = now;
                mode_restore();
                hand_back();                /* the run went without a word (it fell): the dashboard */
                ShowWindow(h, SW_HIDE);
            } else if (now - g_gone_at > FRAME_LINGER_MS) {
                DestroyWindow(h);
            }
        } else if (now - g_t0 > FRAME_WAIT_MS) {
            DestroyWindow(h);   /* the first run never came */
        }
        return 0;
    }
    case WM_DESTROY:
        mode_restore();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

int nc_frame_main(int argc, char **argv)
{
    if (argc < 6) {
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const char *token = argv[2];
    const bool fs = atoi(argv[3]) != 0;
    g_fs60 = atoi(argv[4]) != 0;
    wchar_t title[512];
    if (!MultiByteToWideChar(CP_UTF8, 0, argv[5], -1, title, 512)) {
        wcscpy(title, L"xbased");
    }
    char cls[96];
    snprintf(cls, sizeof(cls), "xbased_frame_%s", token);
    HINSTANCE hi = GetModuleHandleA(NULL);
    wchar_t wcls[96];
    MultiByteToWideChar(CP_UTF8, 0, cls, -1, wcls, 96);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = frame_proc;
    wc.hInstance = hi;
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = wcls;
    if (!RegisterClassExW(&wc)) {
        return 3;
    }
    /* on the dashboard's monitor (the primary one without it), a window's
     * size as the run's own was (1280x960 at that monitor's scale),
     * centered, made smaller (still 4:3) where it wouldn't fit the screen
     * and the taskbar: at 125% or 150% on a 1080p screen it was taller than
     * the screen, its title bar above the top edge, out of reach */
    const char *de = getenv("XBASED_DASH_HWND");
    HWND dash = de && de[0] ? (HWND)(uintptr_t)strtoull(de, NULL, 16) : NULL;
    HMONITOR mon = dash && IsWindow(dash) ? MonitorFromWindow(dash, MONITOR_DEFAULTTOPRIMARY)
                                          : MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoA(mon, &mi);
    const UINT dpi = dash && IsWindow(dash) ? GetDpiForWindow(dash) : GetDpiForSystem();
    const int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
    RECT edge = {0, 0, 0, 0};
    AdjustWindowRectExForDpi(&edge, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const int ew = edge.right - edge.left, eh = edge.bottom - edge.top;
    int cw = MulDiv(1280, (int)dpi, 96), ch = MulDiv(960, (int)dpi, 96);
    const int fit_w = aw * 95 / 100 - ew, fit_h = ah * 95 / 100 - eh;
    if (cw > fit_w || ch > fit_h) {
        if ((int64_t)fit_w * 3 < (int64_t)fit_h * 4) {
            cw = fit_w;
            ch = fit_w * 3 / 4;
        } else {
            ch = fit_h;
            cw = fit_h * 4 / 3;
        }
    }
    const int ww = cw + ew, wh = ch + eh;
    const int x = mi.rcWork.left + (aw - ww) / 2;
    const int y = mi.rcWork.top + (ah - wh) / 2;
    g_fw = CreateWindowExW(0, wcls, title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, ww, wh, NULL, NULL, hi, NULL);
    if (!g_fw) {
        return 4;
    }
    g_t0 = GetTickCount64();
    fp_start(token);    /* the runs' pictures presented here (hw/xbox/hle/vk/share.h) */
    if (fs) {
        set_fullscreen(true);   /* before it's shown: up as a window first, the desktop showed around it */
    }
    /* painted before it's seen: shown cloaked, painted black, then in
     * sight (a window shows nothing until it has painted: what's behind
     * it, the desktop as the dashboard goes, showed through) */
    BOOL cloak = TRUE;
    DwmSetWindowAttribute(g_fw, DWMWA_TRANSITIONS_FORCEDISABLED, &cloak, sizeof(cloak));
    DwmSetWindowAttribute(g_fw, DWMWA_CLOAK, &cloak, sizeof(cloak));
    ShowWindow(g_fw, SW_SHOW);
    UpdateWindow(g_fw);
    cloak = FALSE;
    DwmSetWindowAttribute(g_fw, DWMWA_CLOAK, &cloak, sizeof(cloak));
    SetForegroundWindow(g_fw);
    SetTimer(g_fw, 1, FRAME_TICK_MS, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    mode_restore();
    return 0;
}

/* ------------------------------------------------------------ the game's side */

void *nc_frame_get(const char *title, bool fullscreen, bool fs60)
{
    const char *had = getenv("XBASED_FRAME_HWND");
    if (had && had[0]) {
        HWND h = (HWND)(uintptr_t)strtoull(had, NULL, 16);
        if (IsWindow(h)) {
            return h;     /* the session's, a run started again */
        }
    }
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    char token[48], cmd[MAX_PATH * 2 + 600], t[400];
    snprintf(token, sizeof(token), "%lu_%llu", (unsigned long)GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    snprintf(t, sizeof(t), "%s", title && title[0] ? title : "xbased");
    for (char *p = t; *p; p++) {
        if (*p == '"') {
            *p = '\'';
        }
    }
    snprintf(cmd, sizeof(cmd), "\"%s\" frame %s %d %d \"%s\"", exe, token, fullscreen ? 1 : 0, fs60 ? 1 : 0, t);
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    /* suspended until it may take the foreground: up first, it asked before
     * this process let it, Windows refused and flashed its taskbar button
     * instead (the taskbar over the full screen for a moment) */
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        return NULL;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    char cls[96];
    snprintf(cls, sizeof(cls), "xbased_frame_%s", token);
    HWND h = NULL;
    for (int i = 0; i < 500 && !h; i++) {
        h = FindWindowA(cls, NULL);
        if (!h) {
            if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0) {
                break;  /* it went */
            }
        }
    }
    CloseHandle(pi.hProcess);
    if (h) {
        SetForegroundWindow(h);     /* this process may (the dashboard started it): the frame's in front */
        SetEnvironmentVariableA("XBASED_FRAME_TOKEN", token);
        _putenv_s("XBASED_FRAME_TOKEN", token);
        char v[32];
        snprintf(v, sizeof(v), "%llx", (unsigned long long)(uintptr_t)h);
        SetEnvironmentVariableA("XBASED_FRAME_HWND", v);   /* the runs this one starts: the same frame */
        _putenv_s("XBASED_FRAME_HWND", v);
    }
    return h;
}

static HWND hwnd_of(SDL_Window *w)
{
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
}

void nc_frame_fill(void *frame, SDL_Window *child)
{
    RECT rc;
    GetClientRect((HWND)frame, &rc);
    SetWindowPos(hwnd_of(child), NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

const char *nc_frame_token(void)
{
    return getenv("XBASED_FRAME_TOKEN");
}

void nc_frame_adopt(void *frame, SDL_Window *child, const char *title, bool hidden)
{
    HWND f = (HWND)frame, c = hwnd_of(child);
    wchar_t wt[512];
    if (title && MultiByteToWideChar(CP_UTF8, 0, title, -1, wt, 512)) {
        SendMessageW(f, WM_SETTEXT, 0, (LPARAM)wt);  /* this run's title (another executable's) */
    }
    SetWindowLongPtrA(c, GWL_STYLE, WS_CHILD | WS_CLIPSIBLINGS);
    SetWindowLongPtrA(c, GWL_EXSTYLE, 0);
    SetParent(c, f);
    RECT rc;
    GetClientRect(f, &rc);
    if (hidden) {
        /* 1x1, outside the client area: nothing over the frame's pictures
         * (they keep their independent flip), the keyboard still its */
        SetWindowPos(c, HWND_BOTTOM, -8, -8, 1, 1, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowPos(c, HWND_BOTTOM, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

void nc_frame_raise(void *frame, SDL_Window *child)
{
    HWND f = (HWND)frame, c = hwnd_of(child);
    SetWindowPos(c, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (GetForegroundWindow() == f || GetForegroundWindow() == c) {
        SetFocus(c);
    } else {
        SetForegroundWindow(f);     /* the frame activated: its WM_ACTIVATE hands the keyboard down */
    }
}

void nc_frame_toggle_fullscreen(void *frame)
{
    PostMessageA((HWND)frame, WM_FRAME_TOGGLE, 0, 0);
}

bool nc_frame_minimized(void *frame)
{
    return IsIconic((HWND)frame) != 0;
}

void nc_frame_close(void *frame)
{
    if (frame) {
        PostMessageA((HWND)frame, WM_FRAME_CLOSE, 0, 0);
    }
}

#else /* ------------------------------------------------------------ Linux: X11 */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <dlfcn.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FRAME_TICK_MS   50
#define FRAME_LINGER_MS 3000
#define FRAME_WAIT_MS   10000
#define FRAME_MSG_TOGGLE 1
#define FRAME_MSG_CLOSE  2

/* Xlib's calls by name (SDL loads the library too; no link of our own: a
 * desktop without X11 still starts) */
static struct {
    bool ok;
    int (*ReparentWindow)(Display *, Window, Window, int, int);
    int (*MoveResizeWindow)(Display *, Window, int, int, unsigned, unsigned);
    int (*MapWindow)(Display *, Window);
    int (*RaiseWindow)(Display *, Window);
    int (*LowerWindow)(Display *, Window);
    int (*SetInputFocus)(Display *, Window, int, Time);
    Status (*QueryTree)(Display *, Window, Window *, Window *, Window **, unsigned *);
    int (*Free)(void *);
    Status (*GetWindowAttributes)(Display *, Window, XWindowAttributes *);
    int (*Flush)(Display *);
    Status (*SendEvent)(Display *, Window, Bool, long, XEvent *);
    Atom (*InternAtom)(Display *, const char *, Bool);
    int (*ChangeProperty)(Display *, Window, Atom, Atom, int, int, const unsigned char *, int);
    int (*GetWindowProperty)(Display *, Window, Atom, long, long, Bool, Atom, Atom *, int *, unsigned long *,
                             unsigned long *, unsigned char **);
    int (*StoreName)(Display *, Window, const char *);
    int (*Sync)(Display *, Bool);
    XErrorHandler (*SetErrorHandler)(XErrorHandler);
} X;

static bool x_load(void)
{
    static int tried;
    if (tried) {
        return X.ok;
    }
    tried = 1;
    void *h = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        return false;
    }
#define L(n) (*(void **)&X.n = dlsym(h, "X" #n)) != NULL
    X.ok = L(ReparentWindow) && L(MoveResizeWindow) && L(MapWindow) && L(RaiseWindow) && L(LowerWindow) &&
           L(SetInputFocus) && L(QueryTree) && L(Free) && L(GetWindowAttributes) && L(Flush) && L(SendEvent) &&
           L(InternAtom) && L(ChangeProperty) && L(GetWindowProperty) && L(StoreName) && L(Sync) &&
           L(SetErrorHandler);
#undef L
    return X.ok;
}

/* a window gone meanwhile (the run before's, as it ends): its BadWindow
 * isn't the end of this process, as Xlib's own handler makes it */
static int x_errors(Display *d, XErrorEvent *e)
{
    (void)d;
    (void)e;
    return 0;
}

static uint64_t ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static Display *x_display(SDL_Window *w)
{
    return (Display *)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
}

static Window x_window(SDL_Window *w)
{
    return (Window)SDL_GetNumberProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
}

/* the frame's children, topmost last (QueryTree's order); n returned */
static unsigned children(Display *d, Window f, Window *top)
{
    Window root, parent, *kids = NULL;
    unsigned n = 0;
    if (!X.QueryTree(d, f, &root, &parent, &kids, &n)) {
        n = 0;
    }
    if (top) {
        *top = n ? kids[n - 1] : 0;
    }
    if (kids) {
        X.Free(kids);
    }
    return n;
}

static Atom g_msg_atom, g_min_atom;
static volatile int g_toggle, g_quit;

static bool frame_hook(void *ud, XEvent *e)
{
    (void)ud;
    if (e->type == ClientMessage && e->xclient.message_type == g_msg_atom) {
        if (e->xclient.data.l[0] == FRAME_MSG_TOGGLE) {
            g_toggle = 1;
        } else if (e->xclient.data.l[0] == FRAME_MSG_CLOSE) {
            g_quit = 1;
        }
        return false;
    }
    return true;
}

/* the frame's process: an SDL window of its own on X11, the runs' windows
 * inside it */
static int frame_run(int wfd, pid_t run, bool fs, bool fs60)
{
    signal(SIGPIPE, SIG_IGN);
    if (!SDL_Init(SDL_INIT_VIDEO) || strcmp(SDL_GetCurrentVideoDriver(), "x11") || !x_load()) {
        /* not X11 (Wayland): no frame; the run on its own, this the
         * process the dashboard waits on until it's done */
        if (write(wfd, "0\n", 2) < 0) {
        }
        close(wfd);
        SDL_Quit();
        int st = 0;
        while (waitpid(run, &st, 0) < 0) {
        }
        return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    }
    SDL_Window *w = SDL_CreateWindow("xbased", 1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    Display *d = w ? x_display(w) : NULL;
    const Window fw = w ? x_window(w) : 0;
    if (!d || !fw) {
        if (write(wfd, "0\n", 2) < 0) {
        }
        close(wfd);
        int st = 0;
        while (waitpid(run, &st, 0) < 0) {
        }
        return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    }
    X.SetErrorHandler(x_errors);
    g_msg_atom = X.InternAtom(d, "_XBASED_FRAME", False);
    g_min_atom = X.InternAtom(d, "_XBASED_MINIMIZED", False);
    SDL_SetX11EventHook(frame_hook, NULL);
    if (fs) {
        if (fs60) {
            /* the panel's highest mode a multiple of 60 Hz (win_main.c fs_mode's choice) */
            const SDL_DisplayID dd = SDL_GetDisplayForWindow(w);
            const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(dd);
            int n = 0;
            SDL_DisplayMode **ms = SDL_GetFullscreenDisplayModes(dd, &n);
            const SDL_DisplayMode *best = NULL;
            for (int i = 0; dm && ms && i < n; i++) {
                const int k = (int)(ms[i]->refresh_rate / 60.0f + 0.5f);
                if (ms[i]->w == dm->w && ms[i]->h == dm->h && k >= 1 && k <= 4 &&
                    ms[i]->refresh_rate <= dm->refresh_rate + 0.5f &&
                    SDL_fabsf(ms[i]->refresh_rate - 60.0f * (float)k) <= 0.6f * (float)k &&
                    (!best || ms[i]->refresh_rate > best->refresh_rate)) {
                    best = ms[i];
                }
            }
            const int k0 = dm ? (int)(dm->refresh_rate / 60.0f + 0.5f) : 0;
            const bool already = dm && k0 >= 1 && SDL_fabsf(dm->refresh_rate - 60.0f * (float)k0) <= 0.6f * (float)k0;
            SDL_SetWindowFullscreenMode(w, already ? NULL : best);
            SDL_free(ms);
        }
        SDL_SetWindowFullscreen(w, true);
    }
    char v[32];
    const int vn = snprintf(v, sizeof(v), "%lx\n", (unsigned long)fw);
    if (write(wfd, v, (size_t)vn) < 0) {
    }
    close(wfd);
    const uint64_t t0 = ms_now();
    uint64_t gone = 0;
    bool had = false, shown = false;
    int pw = 0, ph = 0;
    while (!g_quit) {
        SDL_Event e;
        int fitw = 0;
        if (SDL_WaitEventTimeout(&e, FRAME_TICK_MS)) {
            do {
                switch (e.type) {
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
                    /* the window's close: the run's own quit (its window's) */
                    Window root, parent, *kids = NULL;
                    unsigned n = 0;
                    if (X.QueryTree(d, fw, &root, &parent, &kids, &n) && n) {
                        const Atom proto = X.InternAtom(d, "WM_PROTOCOLS", False);
                        const Atom del = X.InternAtom(d, "WM_DELETE_WINDOW", False);
                        for (unsigned i = 0; i < n; i++) {
                            XEvent m;
                            memset(&m, 0, sizeof(m));
                            m.xclient.type = ClientMessage;
                            m.xclient.window = kids[i];
                            m.xclient.message_type = proto;
                            m.xclient.format = 32;
                            m.xclient.data.l[0] = (long)del;
                            m.xclient.data.l[1] = CurrentTime;
                            X.SendEvent(d, kids[i], False, NoEventMask, &m);
                        }
                    } else {
                        g_quit = 1;
                    }
                    if (kids) {
                        X.Free(kids);
                    }
                    break;
                }
                case SDL_EVENT_WINDOW_FOCUS_GAINED: {
                    Window top = 0;
                    XWindowAttributes a;
                    if (children(d, fw, &top) && X.GetWindowAttributes(d, top, &a) && a.map_state == IsViewable) {
                        X.SetInputFocus(d, top, RevertToParent, CurrentTime);    /* the keyboard to the run */
                    }
                    break;
                }
                case SDL_EVENT_WINDOW_MINIMIZED:
                case SDL_EVENT_WINDOW_RESTORED: {
                    const long m = e.type == SDL_EVENT_WINDOW_MINIMIZED;
                    X.ChangeProperty(d, fw, g_min_atom, XA_CARDINAL, 32, PropModeReplace, (const unsigned char *)&m, 1);
                    break;
                }
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                case SDL_EVENT_WINDOW_RESIZED:
                    fitw = 1;
                    break;
                default:
                    break;
                }
            } while (SDL_PollEvent(&e));
        }
        if (g_toggle) {
            g_toggle = 0;
            SDL_SetWindowFullscreen(w, !(SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN));
        }
        int cw = 0, ch = 0;
        SDL_GetWindowSizeInPixels(w, &cw, &ch);
        if (fitw || cw != pw || ch != ph) {
            pw = cw;
            ph = ch;
            Window root, parent, *kids = NULL;
            unsigned n = 0;
            if (X.QueryTree(d, fw, &root, &parent, &kids, &n)) {
                for (unsigned i = 0; i < n; i++) {
                    X.MoveResizeWindow(d, kids[i], 0, 0, (unsigned)(cw > 0 ? cw : 1), (unsigned)(ch > 0 ? ch : 1));
                }
            }
            if (kids) {
                X.Free(kids);
            }
            X.Flush(d);
        }
        const uint64_t now = ms_now();
        if (children(d, fw, NULL)) {
            if (!shown) {
                shown = true;
                SDL_ShowWindow(w);
                SDL_RaiseWindow(w);
            }
            had = true;
            gone = 0;
        } else if (had) {
            if (!gone) {
                gone = now;
                SDL_HideWindow(w);      /* what's behind (the dashboard) at once */
            } else if (now - gone > FRAME_LINGER_MS) {
                break;
            }
        } else if (now - t0 > FRAME_WAIT_MS) {
            break;  /* the first run never came */
        }
        while (waitpid(-1, NULL, WNOHANG) > 0) {
            /* the runs that ended (the first is this process's child) */
        }
    }
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}

void nc_frame_fork(bool fullscreen, bool fs60)
{
    int p[2];
    if (getenv("XBASED_FRAME_XID") || !getenv("DISPLAY") || pipe(p) < 0) {
        return;
    }
    const pid_t run = fork();
    if (run < 0) {
        close(p[0]);
        close(p[1]);
        return;
    }
    if (run > 0) {
        close(p[0]);
        _exit(frame_run(p[1], run, fullscreen, fs60));     /* the parent: the frame */
    }
    close(p[1]);
    /* the run: the frame's window, or "0" for none */
    char v[32] = "";
    struct pollfd pf = {p[0], POLLIN, 0};
    if (poll(&pf, 1, 5000) > 0) {
        const ssize_t n = read(p[0], v, sizeof(v) - 1);
        v[n > 0 ? n : 0] = 0;
    }
    close(p[0]);
    /* "0" too: no frame (Wayland), and the runs started again (exec'd,
     * the same process) don't fork another */
    char s[32];
    snprintf(s, sizeof(s), "%lx", strtoul(v, NULL, 16));
    setenv("XBASED_FRAME_XID", s, 1);
}

void *nc_frame_get(const char *title, bool fullscreen, bool fs60)
{
    (void)title;
    (void)fullscreen;
    (void)fs60;
    const char *v = getenv("XBASED_FRAME_XID");
    const unsigned long xid = v ? strtoul(v, NULL, 16) : 0;
    if (!xid || !x_load() || strcmp(SDL_GetCurrentVideoDriver(), "x11")) {
        return NULL;
    }
    return (void *)(uintptr_t)xid;
}

static SDL_Window *g_child;     /* the run's window, for the calls without it */

void nc_frame_fill(void *frame, SDL_Window *child)
{
    (void)frame;
    (void)child;
}

const char *nc_frame_token(void)
{
    return NULL;
}

void nc_frame_adopt(void *frame, SDL_Window *child, const char *title, bool hidden)
{
    (void)hidden;
    Display *d = x_display(child);
    const Window f = (Window)(uintptr_t)frame, c = x_window(child);
    if (!d || !c) {
        return;
    }
    g_child = child;
    X.SetErrorHandler(x_errors);
    if (title && title[0]) {
        /* this run's title (another executable's) */
        X.ChangeProperty(d, f, X.InternAtom(d, "_NET_WM_NAME", False), X.InternAtom(d, "UTF8_STRING", False), 8,
                         PropModeReplace, (const unsigned char *)title, (int)strlen(title));
        X.StoreName(d, f, title);
    }
    XWindowAttributes a;
    memset(&a, 0, sizeof(a));
    X.GetWindowAttributes(d, f, &a);
    X.ReparentWindow(d, c, f, 0, 0);
    X.MoveResizeWindow(d, c, 0, 0, (unsigned)(a.width > 0 ? a.width : 1280), (unsigned)(a.height > 0 ? a.height : 960));
    X.MapWindow(d, c);
    X.LowerWindow(d, c);    /* under the run before's, until this one has drawn */
    X.Flush(d);
}

void nc_frame_raise(void *frame, SDL_Window *child)
{
    Display *d = x_display(child);
    const Window f = (Window)(uintptr_t)frame, c = x_window(child);
    if (!d || !c) {
        return;
    }
    X.RaiseWindow(d, c);
    XWindowAttributes a;
    if (X.GetWindowAttributes(d, f, &a) && a.map_state == IsViewable && X.GetWindowAttributes(d, c, &a) &&
        a.map_state == IsViewable) {
        X.SetInputFocus(d, c, RevertToParent, CurrentTime);
    }
    X.Flush(d);
}

static void send_msg(void *frame, long what)
{
    Display *d = g_child ? x_display(g_child) : NULL;
    if (!d) {
        return;
    }
    XEvent m;
    memset(&m, 0, sizeof(m));
    m.xclient.type = ClientMessage;
    m.xclient.window = (Window)(uintptr_t)frame;
    m.xclient.message_type = X.InternAtom(d, "_XBASED_FRAME", False);
    m.xclient.format = 32;
    m.xclient.data.l[0] = what;
    X.SendEvent(d, (Window)(uintptr_t)frame, False, NoEventMask, &m);
    X.Flush(d);
}

void nc_frame_toggle_fullscreen(void *frame)
{
    send_msg(frame, FRAME_MSG_TOGGLE);
}

bool nc_frame_minimized(void *frame)
{
    Display *d = g_child ? x_display(g_child) : NULL;
    if (!d) {
        return false;
    }
    Atom type = 0;
    int fmt = 0;
    unsigned long n = 0, left = 0;
    unsigned char *data = NULL;
    bool m = false;
    if (X.GetWindowProperty(d, (Window)(uintptr_t)frame, X.InternAtom(d, "_XBASED_MINIMIZED", False), 0, 1, False,
                            XA_CARDINAL, &type, &fmt, &n, &left, &data) == Success &&
        data && n == 1 && fmt == 32) {
        m = *(long *)data != 0;
    }
    if (data) {
        X.Free(data);
    }
    return m;
}

void nc_frame_close(void *frame)
{
    if (frame) {
        send_msg(frame, FRAME_MSG_CLOSE);
    }
}

#endif
