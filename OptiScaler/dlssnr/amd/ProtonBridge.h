#pragma once
// Proton/Wine gate for the AMD/Neural backend.
//
// Scope: detection + backend selection policy only. All Neural/Pre-SR/Lmxxf
// processing stays in neural-amd-opti. The Linux HIP interop itself lives in
// bridge/proton/ (Linux .so + PE trampoline) and is reached through the stock
// LoadLibrary path as amdhip64_7.dll under Wine.
//
// Daniel stays Windows-only: it needs CreateThread filtering, Toolhelp,
// VirtualProtect/IAT patching and NT-only flows (RuntimeHostLoad.h,
// AmdLayout.h) that do not exist under Wine.

#include <windows.h>

namespace DlssNr::Proton
{
// True when running under Wine/Proton (ntdll exports wine_get_unix_env).
// GetProcAddress on the loaded ntdll; no side effects, safe to call early.
inline bool IsWine()
{
    static int cached = -1;
    if (cached >= 0)
        return cached != 0;
    bool wine = false;
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
        wine = GetProcAddress(ntdll, "wine_get_unix_env") != nullptr;
    cached = wine ? 1 : 0;
    return wine;
}

// Optional explicit HIP device index for Proton/Linux where the D3D12 LUID
// has no R0600 counterpart. LMXXF_HIP_DEVICE=N (or DLSSNR_HIP_DEVICE=N).
// Returns -1 when unset/invalid.
inline int HipDeviceOverride()
{
    char buf[16] {};
    DWORD n = GetEnvironmentVariableA("LMXXF_HIP_DEVICE", buf, sizeof(buf));
    if (!n || n >= sizeof(buf))
        n = GetEnvironmentVariableA("DLSSNR_HIP_DEVICE", buf, sizeof(buf));
    if (!n || n >= sizeof(buf) || !buf[0])
        return -1;
    char* end = nullptr;
    const long v = strtol(buf, &end, 10);
    if (end == buf || *end != '\0' || v < 0 || v > 64)
        return -1;
    return static_cast<int>(v);
}
} // namespace DlssNr::Proton
