/* PE trampoline: amdhip64_7.dll exports (Win64) -> Linux HIP via
 * NeuralAmdHipBridge published by bridge/proton/hip_bridge_linux.c.
 *
 * This file is the Wine-side half. It is NOT AMD's runtime: every export
 * resolves the bridge pointer via __wine_get_unix_env(NEURAL_AMD_HIP_ENV)
 * (live Unix environ from LD_PRELOAD) and forwards with the SysV ABI.
 *
 * Coverage matches third_party/lmxxf/Development/HIP/hip_api.h (the Lmxxf
 * path): runtime, memory, stream/event, external memory + semaphore, module
 * load/launch. Fatbin registration (__hipRegister*) is intentionally absent:
 * Lmxxf loads precompiled .hsaco via hipModuleLoadData.
 *
 * Build (example): clang -target x86_64-pc-windows-msvc -c ... + lld-link
 *   /DLL /OUT:amdhip64_7.dll ...  (see Makefile). The DLL is placed in the
 *   Proton prefix drive_c/windows/system32; OptiScaler/LmxxfNrRuntime keep
 *   calling LoadLibraryExW("amdhip64_7.dll") unchanged.
 */
#include "hip_bridge_shared.h"

#define EXPORT __declspec(dllexport)
#define SYSV __attribute__((sysv_abi))

typedef int BOOL;
typedef unsigned long DWORD;
typedef void *HANDLE;
#define DLL_PROCESS_ATTACH 1

__declspec(dllimport) DWORD __stdcall GetEnvironmentVariableA(const char *name, char *buf, DWORD size);
__declspec(dllimport) int __stdcall __wine_get_unix_env(const char *var, char *buf, unsigned long long size);

static NeuralAmdHipBridge *g;

static unsigned long long parse_hex_ptr(const char *s) {
    unsigned long long v = 0;
    if (!s)
        return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            break;
    }
    return v;
}

static int bridge_ok(void) {
    return g && g->magic == NEURAL_AMD_HIP_MAGIC;
}

static void init_bridge(void) {
    char buf[64];
    buf[0] = 0;
    if (__wine_get_unix_env(NEURAL_AMD_HIP_ENV, buf, sizeof(buf)) != 0 || !buf[0]) {
        DWORD n = GetEnvironmentVariableA(NEURAL_AMD_HIP_ENV, buf, (DWORD)sizeof(buf));
        if (n == 0 || n >= sizeof(buf))
            buf[0] = 0;
    }
    if (buf[0])
        g = (NeuralAmdHipBridge *)(unsigned long long)parse_hex_ptr(buf);
}

BOOL __stdcall DllMain(void *h, DWORD reason, void *r) {
    (void)h;
    (void)r;
    if (reason == DLL_PROCESS_ATTACH)
        init_bridge();
    return 1;
}

#define HIP_ERROR_NOT_INITIALIZED 3
#define HIP_SUCCESS 0

#define FWD0(ret, name) \
    EXPORT ret name(void) { \
        if (!bridge_ok() || !g->p_##name) \
            return (ret)HIP_ERROR_NOT_INITIALIZED; \
        return ((ret SYSV (*)(void))g->p_##name)(); \
    }
#define FWD1(ret, name, T1) \
    EXPORT ret name(T1 a) { \
        if (!bridge_ok() || !g->p_##name) \
            return (ret)HIP_ERROR_NOT_INITIALIZED; \
        return ((ret SYSV (*)(T1))g->p_##name)(a); \
    }
#define FWD2(ret, name, T1, T2) \
    EXPORT ret name(T1 a, T2 b) { \
        if (!bridge_ok() || !g->p_##name) \
            return (ret)HIP_ERROR_NOT_INITIALIZED; \
        return ((ret SYSV (*)(T1, T2))g->p_##name)(a, b); \
    }
#define FWD3(ret, name, T1, T2, T3) \
    EXPORT ret name(T1 a, T2 b, T3 c) { \
        if (!bridge_ok() || !g->p_##name) \
            return (ret)HIP_ERROR_NOT_INITIALIZED; \
        return ((ret SYSV (*)(T1, T2, T3))g->p_##name)(a, b, c); \
    }
#define FWD4(ret, name, T1, T2, T3, T4) \
    EXPORT ret name(T1 a, T2 b, T3 c, T4 d) { \
        if (!bridge_ok() || !g->p_##name) \
            return (ret)HIP_ERROR_NOT_INITIALIZED; \
        return ((ret SYSV (*)(T1, T2, T3, T4))g->p_##name)(a, b, c, d); \
    }
#define FWD5(ret, name, T1, T2, T3, T4, T5) \
    EXPORT ret name(T1 a, T2 b, T3 c, T4 d, T5 e) { \
        if (!bridge_ok() || !g->p_##name) \
            return (ret)HIP_ERROR_NOT_INITIALIZED; \
        return ((ret SYSV (*)(T1, T2, T3, T4, T5))g->p_##name)(a, b, c, d, e); \
    }

FWD1(int, hipInit, unsigned)
FWD1(int, hipRuntimeGetVersion, int *)
FWD1(int, hipDriverGetVersion, int *)
FWD1(int, hipGetDeviceCount, int *)
FWD1(int, hipSetDevice, int)
FWD2(int, hipGetDevicePropertiesR0600, void *, int)
FWD3(int, hipDeviceGetName, char *, int, int)
FWD2(int, hipMemGetInfo, unsigned long *, unsigned long *)
FWD2(int, hipMalloc, void **, unsigned long)
FWD1(int, hipFree, void *)
FWD3(int, hipHostMalloc, void **, unsigned long, unsigned)
FWD4(int, hipMemcpy, void *, const void *, unsigned long, int)
FWD5(int, hipMemcpyAsync, void *, const void *, unsigned long, int, void *)
FWD4(int, hipMemsetAsync, void *, int, unsigned long, void *)
FWD1(int, hipStreamCreate, void **)
FWD1(int, hipStreamSynchronize, void *)
FWD1(int, hipStreamDestroy, void *)
FWD0(int, hipDeviceSynchronize)
FWD1(int, hipEventCreate, void **)
FWD2(int, hipEventRecord, void *, void *)
FWD3(int, hipEventElapsedTime, float *, void *, void *)
FWD1(int, hipEventDestroy, void *)
FWD1(int, hipEventSynchronize, void *)
FWD2(int, hipImportExternalMemory, void **, const void *)
FWD3(int, hipExternalMemoryGetMappedBuffer, void **, void *, const void *)
FWD1(int, hipDestroyExternalMemory, void *)
FWD2(int, hipImportExternalSemaphore, void **, const void *)
FWD1(int, hipDestroyExternalSemaphore, void *)
FWD2(int, hipModuleLoadData, void **, const void *)
FWD2(int, hipModuleLoad, void **, const char *)
FWD3(int, hipModuleGetFunction, void **, void *, const char *)
FWD1(int, hipModuleUnload, void *)

EXPORT int hipSignalExternalSemaphoresAsync(const void *sems, const void *params, unsigned n,
                                            void *stream) {
    if (!bridge_ok() || !g->p_hipSignalExternalSemaphoresAsync)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(const void *, const void *, unsigned, void *))
                g->p_hipSignalExternalSemaphoresAsync)(sems, params, n, stream);
}

EXPORT int hipWaitExternalSemaphoresAsync(const void *sems, const void *params, unsigned n,
                                          void *stream) {
    if (!bridge_ok() || !g->p_hipWaitExternalSemaphoresAsync)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(const void *, const void *, unsigned, void *))
                g->p_hipWaitExternalSemaphoresAsync)(sems, params, n, stream);
}

EXPORT int hipModuleLaunchKernel(void *f, unsigned bx, unsigned by, unsigned bz, unsigned tx,
                                 unsigned ty, unsigned tz, unsigned sh, void *stream, void **args,
                                 void **extra) {
    if (!bridge_ok() || !g->p_hipModuleLaunchKernel)
        return HIP_ERROR_NOT_INITIALIZED;
    return ((int SYSV (*)(void *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                          unsigned, void *, void **, void **))g->p_hipModuleLaunchKernel)(
        f, bx, by, bz, tx, ty, tz, sh, stream, args, extra);
}

EXPORT const char *hipGetErrorName(int e) {
    if (!bridge_ok() || !g->p_hipGetErrorName)
        return "hip bridge not initialized";
    return ((const char *SYSV (*)(int))g->p_hipGetErrorName)(e);
}
