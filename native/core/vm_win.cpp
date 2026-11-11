/*
 * hakuX native core — guest memory on Windows (core/vm.c's host side).
 *
 * The guest's address space is the low 4 GiB of the process itself: guest
 * VA x is host address x (vm->base is null), which the translator relies on
 * (cpu/cpu_x86.cpp). The process keeps its own memory above 4 GiB
 * (high-entropy ASLR), and the low 4 GiB are reserved as placeholders before
 * anything else can land there. Guest RAM is one pagefile-backed section,
 * mapped page-granular at every guest address that reaches it: views of 4
 * KiB at 4 KiB file offsets replace placeholders (MapViewOfFile3 with
 * MEM_REPLACE_PLACEHOLDER, Windows 10 1803 up), as Linux's mmap of a memfd
 * does on Android.
 *
 * Windows wants a view replaced or protected whole, so the regions (views
 * and placeholders) are kept in a map: mapping over part of a view unmaps
 * it, splits the placeholder left behind and maps the parts that stay
 * again, with their protection (vm.c's nc_vm_reprotect).
 */
#include "native/core/vm.h"

#include <windows.h>

#include <map>

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif

extern "C" void nc_vm_reprotect(NcVm *vm, uint32_t va, uint32_t len);

namespace {

typedef PVOID(WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID(WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef BOOL(WINAPI *UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);

VirtualAlloc2Fn p_VirtualAlloc2;
MapViewOfFile3Fn p_MapViewOfFile3;
UnmapViewOfFile2Fn p_UnmapViewOfFile2;

struct Region {
    uint32_t pages;
    bool view;
    uint32_t pa;    /* a view's first physical address */
};

/* by first page; covers the reserved low 4 GiB */
std::map<uint32_t, Region> g_regions;
HANDLE g_section;
NcVm *g_vm;
SRWLOCK g_lock = SRWLOCK_INIT;

void *addr(uint32_t page)
{
    return (void *)((uintptr_t)page << 12);
}

std::map<uint32_t, Region>::iterator region_of(uint32_t page)
{
    auto it = g_regions.upper_bound(page);
    if (it == g_regions.begin()) {
        return g_regions.end();
    }
    --it;
    return page < it->first + it->second.pages ? it : g_regions.end();
}

bool map_view(uint32_t page, uint32_t pages, uint32_t pa)
{
    void *want = addr(page);
    void *m = p_MapViewOfFile3(g_section, GetCurrentProcess(), want, pa, (SIZE_T)pages << 12,
                               MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (m != want) {
        nc_log("hakuX-native", "vm: view at %08x (pa %08x, %u pages) failed: %lu", page << 12, pa, pages,
               GetLastError());
        return false;
    }
    return true;
}

/* A view back to a placeholder of the same extent. */
bool unmap_view(std::map<uint32_t, Region>::iterator it)
{
    if (!p_UnmapViewOfFile2(GetCurrentProcess(), addr(it->first), MEM_PRESERVE_PLACEHOLDER)) {
        nc_log("hakuX-native", "vm: unmapping the view at %08x failed: %lu", it->first << 12, GetLastError());
        return false;
    }
    it->second.view = false;
    return true;
}

/* page becomes a region boundary. */
bool split(uint32_t page)
{
    auto it = region_of(page);
    if (it == g_regions.end() || it->first == page) {
        return true;
    }
    const uint32_t start = it->first;
    const Region r = it->second;
    if (r.view && !unmap_view(it)) {
        return false;
    }
    if (!VirtualFree(addr(start), (SIZE_T)(page - start) << 12, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        nc_log("hakuX-native", "vm: splitting the placeholder at %08x at %08x failed: %lu", start << 12, page << 12,
               GetLastError());
        return false;
    }
    it->second.pages = page - start;
    g_regions[page] = Region{r.pages - (page - start), false, 0};
    if (r.view) {
        /* both parts mapped again, as they were */
        const uint32_t pa2 = r.pa + ((page - start) << 12);
        if (!map_view(start, page - start, r.pa) || !map_view(page, r.pages - (page - start), pa2)) {
            return false;
        }
        it->second.view = true;
        it->second.pa = r.pa;
        g_regions[page] = Region{r.pages - (page - start), true, pa2};
        nc_vm_reprotect(g_vm, start << 12, r.pages << 12);
    }
    return true;
}

/* The player's words when Windows' memory holds the guest's addresses: the
 * process runs with bottom-up ASLR on (app/win_aslr.c) unless something
 * keeps it off. */
const char k_low_taken[] =
    "Windows put other memory where the Xbox's memory has to go. Usually that's \"Randomize memory allocations "
    "(Bottom-up ASLR)\" turned off in Windows Security > App & browser control > Exploit protection. Turning it "
    "back on (or on for xbased.exe in Program settings) fixes it.";

/* What holds host memory in the low 4 GiB, for the log: a DLL by its file,
 * a mapped file by its name, private memory by its size. */
void log_taken(const MEMORY_BASIC_INFORMATION &mbi, uint64_t lo, uint64_t hi)
{
    char what[MAX_PATH * 3] = "";
    if (mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED) {
        wchar_t w[MAX_PATH] = L"";
        DWORD n = 0;
        if (mbi.Type == MEM_IMAGE) {
            n = GetModuleFileNameW((HMODULE)mbi.AllocationBase, w, MAX_PATH);
        }
        if (!n) {
            typedef DWORD(WINAPI * MappedFn)(HANDLE, LPVOID, LPWSTR, DWORD);
            static MappedFn mapped = (MappedFn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                      "K32GetMappedFileNameW");
            n = mapped ? mapped(GetCurrentProcess(), (void *)(uintptr_t)lo, w, MAX_PATH) : 0;
        }
        if (n) {
            WideCharToMultiByte(CP_UTF8, 0, w, -1, what, sizeof(what), nullptr, nullptr);
        }
    }
    nc_log("hakuX-native", "vm: host memory at %08llx-%08llx (%s %s) %s", (unsigned long long)lo,
           (unsigned long long)hi, mbi.State == MEM_RESERVE ? "reserved" : "committed",
           mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped" : "private", what);
}

} // namespace

extern "C" bool nc_vmos_init(NcVm *vm, uint32_t ram_size)
{
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    p_VirtualAlloc2 = (VirtualAlloc2Fn)(void *)GetProcAddress(kb, "VirtualAlloc2");
    p_MapViewOfFile3 = (MapViewOfFile3Fn)(void *)GetProcAddress(kb, "MapViewOfFile3");
    p_UnmapViewOfFile2 = (UnmapViewOfFile2Fn)(void *)GetProcAddress(kb, "UnmapViewOfFile2");
    if (!p_VirtualAlloc2 || !p_MapViewOfFile3 || !p_UnmapViewOfFile2) {
        nc_log("hakuX-native", "vm: Windows 10 1803 or later is needed (no placeholder API)");
        return false;
    }
    g_vm = vm;

    /* Every free range of the low 4 GiB becomes a placeholder. */
    uint64_t at = 0x10000;
    unsigned ranges = 0, taken = 0;
    uint64_t reserved = 0;
    while (at < (1ull << 32)) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)at, &mbi, sizeof(mbi))) {
            break;
        }
        uint64_t lo = (uint64_t)(uintptr_t)mbi.BaseAddress, hi = lo + mbi.RegionSize;
        if (hi > (1ull << 32)) {
            hi = 1ull << 32;
        }
        if (mbi.State == MEM_FREE) {
            const uint64_t a = (lo + 0xFFFF) & ~0xFFFFull, b = hi & ~0xFFFFull;
            if (b > a) {
                void *p = p_VirtualAlloc2(GetCurrentProcess(), (void *)(uintptr_t)a, (SIZE_T)(b - a),
                                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
                if (p != (void *)(uintptr_t)a) {
                    nc_log("hakuX-native", "vm: reserving %08llx-%08llx failed: %lu", (unsigned long long)a,
                           (unsigned long long)b, GetLastError());
                    return false;
                }
                g_regions[(uint32_t)(a >> 12)] = Region{(uint32_t)((b - a) >> 12), false, 0};
                ranges++;
                reserved += b - a;
            }
        } else if (!(lo >= 0x7FFE0000ull && hi <= 0x7FFF0000ull) && taken++ < 16) {
            log_taken(mbi, lo, hi);     /* Windows' shared pages aside, in every process */
        }
        if (mbi.State != MEM_FREE && lo < 0x80000000ull + 0x08000000ull && hi > 0x80000000ull) {
            nc_log("hakuX-native", "vm: host memory at %08llx-%08llx, inside the guest's RAM window",
                   (unsigned long long)lo, (unsigned long long)hi);
            nc_vm_why = k_low_taken;
            return false;
        }
        at = hi;
    }
    if (taken > 16) {
        nc_log("hakuX-native", "vm: and %u more", taken - 16);
    }
    nc_log("hakuX-native", "vm: %llu MiB of the low 4 GiB reserved for the guest (%u ranges)",
           (unsigned long long)(reserved >> 20), ranges);

    /* The 4 GiB above the guest's stay empty too, as far as they are free.
     * Host code reaches guest memory as address + length, both the guest's
     * (a kernel call's buffer and its size): at most 4 GiB past the guest's
     * last byte. Nothing of the process may ever be there to be read or
     * written that way: with these reserved, such an access faults and is
     * reported as the title's. */
    uint64_t guard = 0;
    for (at = 1ull << 32; at < (2ull << 32);) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)at, &mbi, sizeof(mbi))) {
            break;
        }
        const uint64_t lo = (uint64_t)(uintptr_t)mbi.BaseAddress, end = lo + mbi.RegionSize;
        const uint64_t hi = end < (2ull << 32) ? end : (2ull << 32);
        const uint64_t a = (lo + 0xFFFF) & ~0xFFFFull, b = hi & ~0xFFFFull;
        if (mbi.State == MEM_FREE && b > a &&
            VirtualAlloc((void *)(uintptr_t)a, (SIZE_T)(b - a), MEM_RESERVE, PAGE_NOACCESS) == (void *)(uintptr_t)a) {
            guard += b - a;
        }
        at = end;
    }
    if (guard != (1ull << 32)) {
        nc_log("hakuX-native", "vm: %llu MiB of the 4 GiB above the guest's kept empty (the rest is the process's)",
               (unsigned long long)(guard >> 20));
    }

    g_section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, ram_size, nullptr);
    if (!g_section) {
        nc_log("hakuX-native", "vm: guest RAM section failed: %lu", GetLastError());
        return false;
    }
    vm->reservation = nullptr;
    vm->reservation_len = 1ull << 32;
    vm->base = nullptr;
    vm->ram_fd = -1;
    void *phys = MapViewOfFile(g_section, FILE_MAP_ALL_ACCESS, 0, 0, ram_size);
    if (!phys) {
        nc_log("hakuX-native", "vm: RAM view failed: %lu", GetLastError());
        return false;
    }
    vm->phys = (uint8_t *)phys;
    return true;
}

extern "C" bool nc_vmos_map(NcVm *vm, uint32_t va, uint32_t pa, uint32_t len)
{
    AcquireSRWLockExclusive(&g_lock);
    const uint32_t s = va >> 12, e = (uint32_t)(((uint64_t)va + len) >> 12);
    bool ok = split(s) && (e >= (1u << 20) || split(e));
    unsigned n = 0;
    for (auto it = g_regions.find(s); ok && it != g_regions.end() && it->first < e; ++it, n++) {
        if (it->second.view) {
            ok = unmap_view(it);
        }
    }
    if (ok && region_of(s) == g_regions.end()) {
        nc_log("hakuX-native", "vm: %08x is outside the guest's reservation", va);
        nc_vm_why = k_low_taken;
        ok = false;
    }
    if (ok && n > 1) {
        if (!VirtualFree(addr(s), (SIZE_T)(e - s) << 12, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
            nc_log("hakuX-native", "vm: coalescing %08x+%x failed: %lu", va, len, GetLastError());
            ok = false;
        } else {
            g_regions.erase(g_regions.upper_bound(s), g_regions.lower_bound(e));
            g_regions[s].pages = e - s;
        }
    }
    if (ok) {
        ok = map_view(s, e - s, pa);
        if (ok) {
            g_regions[s] = Region{e - s, true, pa};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

extern "C" bool nc_vmos_unmap(NcVm *vm, uint32_t va, uint32_t len)
{
    AcquireSRWLockExclusive(&g_lock);
    const uint32_t s = va >> 12, e = (uint32_t)(((uint64_t)va + len) >> 12);
    bool ok = split(s) && (e >= (1u << 20) || split(e));
    for (auto it = g_regions.find(s); ok && it != g_regions.end() && it->first < e; ++it) {
        if (it->second.view) {
            ok = unmap_view(it);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

/* prot over guest [va, va + len): each view's part on its own (Windows
 * changes protection within one view at a time). */
extern "C" void nc_vmos_protect(NcVm *vm, uint32_t va, uint32_t len, int prot)
{
    const DWORD wp = prot & 2 ? PAGE_READWRITE : prot & 1 ? PAGE_READONLY : PAGE_NOACCESS;
    const uint32_t s = va >> 12, e = (uint32_t)(((uint64_t)va + len + 0xFFF) >> 12);
    AcquireSRWLockShared(&g_lock);
    auto it = region_of(s);
    if (it == g_regions.end()) {
        it = g_regions.lower_bound(s);
    }
    for (; it != g_regions.end() && it->first < e; ++it) {
        if (!it->second.view) {
            continue;
        }
        const uint32_t a = it->first > s ? it->first : s;
        const uint32_t b = it->first + it->second.pages < e ? it->first + it->second.pages : e;
        DWORD old;
        if (b > a && !VirtualProtect(addr(a), (SIZE_T)(b - a) << 12, wp, &old)) {
            static unsigned logged;
            if (logged++ < 8) {
                nc_log("hakuX-native", "vm: protecting %08x+%x failed: %lu", a << 12, (b - a) << 12, GetLastError());
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
}
