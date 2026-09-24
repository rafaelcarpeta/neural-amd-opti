/* Shared layout between the Linux HIP bridge (.so) and the PE amdhip64_7.dll
 * trampoline used under Wine/Proton.
 *
 * Scope: Lmxxf/AMD backend of neural-amd-opti only. No Neural/Pre-SR logic,
 * no launcher/deploy, no vkd3d patch, no Daniel flag-kernel rendezvous.
 * The PE side keeps calling the stock Windows HIP ABI (win32 handles,
 * type=5/4); the Linux side converts NT handles to dma-buf fds (OpaqueFd)
 * and forwards everything else to libamdhip64.so.7.
 */
#ifndef NEURAL_AMD_PROTON_BRIDGE_H
#define NEURAL_AMD_PROTON_BRIDGE_H

#define NEURAL_AMD_HIP_MAGIC 0x314849504E524C44ULL /* "DLNRHIP1" */
#define NEURAL_AMD_HIP_ENV "NEURAL_AMD_HIP_BRIDGE"

typedef struct {
    unsigned long long magic;
    /* runtime/device */
    int (*p_hipInit)(unsigned);
    int (*p_hipRuntimeGetVersion)(int *);
    int (*p_hipDriverGetVersion)(int *);
    int (*p_hipGetDeviceCount)(int *);
    int (*p_hipSetDevice)(int);
    int (*p_hipGetDevicePropertiesR0600)(void *, int);
    int (*p_hipDeviceGetName)(char *, int, int);
    int (*p_hipMemGetInfo)(unsigned long *, unsigned long *);
    /* memory */
    int (*p_hipMalloc)(void **, unsigned long);
    int (*p_hipFree)(void *);
    int (*p_hipHostMalloc)(void **, unsigned long, unsigned);
    int (*p_hipMemcpy)(void *, const void *, unsigned long, int);
    int (*p_hipMemcpyAsync)(void *, const void *, unsigned long, int, void *);
    int (*p_hipMemsetAsync)(void *, int, unsigned long, void *);
    /* stream/event/sync */
    int (*p_hipStreamCreate)(void **);
    int (*p_hipStreamSynchronize)(void *);
    int (*p_hipStreamDestroy)(void *);
    int (*p_hipDeviceSynchronize)(void);
    int (*p_hipEventCreate)(void **);
    int (*p_hipEventRecord)(void *, void *);
    int (*p_hipEventElapsedTime)(float *, void *, void *);
    int (*p_hipEventDestroy)(void *);
    int (*p_hipEventSynchronize)(void *);
    /* external memory (D3D12 shared buffers) */
    int (*p_hipImportExternalMemory)(void **, const void *);
    int (*p_hipExternalMemoryGetMappedBuffer)(void **, void *, const void *);
    int (*p_hipDestroyExternalMemory)(void *);
    /* external semaphore (D3D12 shared fence) */
    int (*p_hipImportExternalSemaphore)(void **, const void *);
    int (*p_hipSignalExternalSemaphoresAsync)(const void *, const void *, unsigned, void *);
    int (*p_hipWaitExternalSemaphoresAsync)(const void *, const void *, unsigned, void *);
    int (*p_hipDestroyExternalSemaphore)(void *);
    /* modules (precompiled .hsaco via hipModuleLoadData) */
    int (*p_hipModuleLoadData)(void **, const void *);
    int (*p_hipModuleLoad)(void **, const char *);
    int (*p_hipModuleGetFunction)(void **, void *, const char *);
    int (*p_hipModuleLaunchKernel)(void *, unsigned, unsigned, unsigned, unsigned, unsigned,
                                  unsigned, unsigned, void *, void **, void **);
    int (*p_hipModuleUnload)(void *);
    const char *(*p_hipGetErrorName)(int);
} NeuralAmdHipBridge;

#endif
