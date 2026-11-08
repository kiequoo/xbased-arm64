/*
 * xbased — the guest's memory and Windows' address space randomization
 * (win_aslr.h).
 */
#ifdef _WIN32
#include "native/app/win_aslr.h"

#include <stdint.h>
#include <stdio.h>

#ifndef PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON
#define PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON (0x00000001ull << 16)
#endif
#ifndef PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON
#define PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON (0x00000001ull << 20)
#endif

bool xb_spawn(const char *exe, char *cmd, const char *dir, PROCESS_INFORMATION *pi)
{
    STARTUPINFOEXA si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    char buf[256];
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    LPPROC_THREAD_ATTRIBUTE_LIST list = size <= sizeof(buf) ? (LPPROC_THREAD_ATTRIBUTE_LIST)buf : NULL;
    DWORD64 policy = PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON |
                     PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON;
    if (list && InitializeProcThreadAttributeList(list, 1, 0, &size)) {
        if (UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &policy, sizeof(policy),
                                      NULL, NULL)) {
            si.lpAttributeList = list;
        }
    }
    bool ok = false;
    if (si.lpAttributeList) {
        ok = CreateProcessA(exe, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, dir, &si.StartupInfo,
                            pi);
    }
    if (!ok) {
        /* a Windows that doesn't take the attribute: as before */
        si.StartupInfo.cb = sizeof(STARTUPINFOA);
        ok = CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si.StartupInfo, pi);
    }
    if (si.lpAttributeList) {
        DeleteProcThreadAttributeList(list);
    }
    return ok;
}

bool xb_aslr_low_taken(void)
{
    uint64_t at = 0x10000;
    while (at < (1ull << 32)) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)at, &mbi, sizeof(mbi))) {
            return false;
        }
        const uint64_t lo = (uint64_t)(uintptr_t)mbi.BaseAddress, hi = lo + mbi.RegionSize;
        /* KUSER_SHARED_DATA and its neighbour, in every process, are no
         * part of the guest's (vm_win.cpp reserves around them) */
        if (mbi.State != MEM_FREE && !(lo >= 0x7FFE0000ull && hi <= 0x7FFF0000ull)) {
            return true;
        }
        at = hi;
    }
    return false;
}

void xb_aslr_check(void)
{
    if (!xb_aslr_low_taken()) {
        return;
    }
    PROCESS_MITIGATION_ASLR_POLICY aslr;
    if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessASLRPolicy, &aslr, sizeof(aslr)) &&
        aslr.EnableBottomUpRandomization && aslr.EnableHighEntropy) {
        return;     /* on already: the boot says what holds the memory (vm_win.cpp) */
    }
    char exe[MAX_PATH];
    if (GetEnvironmentVariableA("XBASED_ASLR_RUN", NULL, 0) || !GetModuleFileNameA(NULL, exe, sizeof(exe))) {
        return;     /* the run started again already: once */
    }
    SetEnvironmentVariableA("XBASED_ASLR_RUN", "1");
    static char cmd[32768];
    snprintf(cmd, sizeof(cmd), "%s", GetCommandLineA());
    PROCESS_INFORMATION pi;
    if (!xb_spawn(exe, cmd, NULL, &pi)) {
        return;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    TerminateProcess(GetCurrentProcess(), code);
}
#endif
