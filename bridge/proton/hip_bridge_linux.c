/* Linux side of the Proton HIP bridge for the Lmxxf backend.
 *
 * Loaded via LD_PRELOAD inside the Wine/Proton process. On load it dlopens
 * libamdhip64.so.7 (DLSSNR_HIP_LIBRARY / LMXXF_HIP_LIBRARY first, then
 * /opt/rocm/lib + soname fallbacks), resolves the runtime ABI, and publishes
 * the table address in NEURAL_AMD_HIP_BRIDGE for the PE amdhip64_7.dll
 * trampoline (which reads it via __wine_get_unix_env).
 *
 * Only interop needs special handling: the PE side passes Windows
 * hipExternalMemoryHandleDesc / hipExternalSemaphoreHandleDesc with win32
 * handles (type 5/4). Linux HIP needs OpaqueFd (type 1). Conversion uses
 * wine_server_handle_to_fd, falling back to win32u.so d3dkmt helpers resolved
 * as local (non-dynsym) ELF symbols. Everything else is passthrough.
 *
 * Build: see Makefile (cc -std=gnu11 -O2 -fPIC -shared -pthread).
 */
#define _GNU_SOURCE
#include "hip_bridge_shared.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static NeuralAmdHipBridge g_bridge;
static void *g_hip;
static int g_hip_ok;
static FILE *g_log;

static void blog(const char *fmt, ...) {
    va_list ap;
    if (!g_log) {
        const char *path = getenv("NEURAL_AMD_HIP_LOG");
        if (!path || !path[0])
            path = getenv("DLSSNR_HIP_LOG");
        g_log = fopen(path && path[0] ? path : "/tmp/neural-amd-hip.log", "a");
        if (g_log)
            setvbuf(g_log, NULL, _IOLBF, 0);
    }
    if (!g_log)
        return;
    fprintf(g_log, "[pid %d] ", (int)getpid());
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
}

static void *must_dlsym(const char *name) {
    void *p = dlsym(g_hip, name);
    if (!p)
        blog("missing %s: %s", name, dlerror());
    return p;
}

static void hip_preload_needed(const char *hip_path) {
    (void)hip_path;
    /* ROCm wheels carry DT_NEEDED entries next to the HIP lib; the generic
     * loader resolves them via rpath/runpath. No Wine-specific preloading
     * needed for the passthrough path. Kept as a hook for distro quirks. */
}

static int ensure_hip(void) {
    if (g_hip_ok)
        return 0;
    if (g_hip)
        return -1;
    const char *env = getenv("DLSSNR_HIP_LIBRARY");
    if ((!env || !env[0]))
        env = getenv("LMXXF_HIP_LIBRARY");
    if (env && env[0]) {
        hip_preload_needed(env);
        g_hip = dlopen(env, RTLD_NOW | RTLD_GLOBAL);
        if (!g_hip) {
            blog("dlopen selected HIP %s failed: %s", env, dlerror());
            return -1;
        }
        blog("dlopen selected HIP %s", env);
    } else {
        static const char *const candidates[] = {
            "/opt/rocm/lib/libamdhip64.so.7",
            "libamdhip64.so.7",
            "libamdhip64.so",
            NULL,
        };
        for (int i = 0; candidates[i]; i++) {
            g_hip = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
            if (g_hip) {
                blog("dlopen %s", candidates[i]);
                break;
            }
        }
    }
    if (!g_hip) {
        blog("dlopen libamdhip64 failed: %s", dlerror());
        return -1;
    }
#define R(name) g_bridge.p_##name = must_dlsym(#name)
    R(hipInit);
    R(hipRuntimeGetVersion);
    R(hipDriverGetVersion);
    R(hipGetDeviceCount);
    R(hipSetDevice);
    R(hipGetDevicePropertiesR0600);
    R(hipDeviceGetName);
    R(hipMemGetInfo);
    R(hipMalloc);
    R(hipFree);
    R(hipHostMalloc);
    R(hipMemcpy);
    R(hipMemcpyAsync);
    R(hipMemsetAsync);
    R(hipStreamCreate);
    R(hipStreamSynchronize);
    R(hipStreamDestroy);
    R(hipDeviceSynchronize);
    R(hipEventCreate);
    R(hipEventRecord);
    R(hipEventElapsedTime);
    R(hipEventDestroy);
    R(hipEventSynchronize);
    R(hipImportExternalMemory);
    R(hipExternalMemoryGetMappedBuffer);
    R(hipDestroyExternalMemory);
    R(hipImportExternalSemaphore);
    R(hipSignalExternalSemaphoresAsync);
    R(hipWaitExternalSemaphoresAsync);
    R(hipDestroyExternalSemaphore);
    R(hipModuleLoadData);
    R(hipModuleLoad);
    R(hipModuleGetFunction);
    R(hipModuleLaunchKernel);
    R(hipModuleUnload);
    g_bridge.p_hipGetErrorName = must_dlsym("hipGetErrorName");
#undef R
    g_hip_ok = 1;
    return 0;
}

/* MSVC layout of the external-memory/semaphore descs (104/96 bytes). */
struct win_extmem_desc {
    unsigned int type;
    unsigned int pad;
    union {
        int fd;
        struct {
            void *handle;
            const void *name;
        } win32;
    } handle;
    unsigned long long size;
    unsigned int flags;
    unsigned int reserved[16];
};

enum {
    HIP_MEM_OPAQUE_FD = 1,
    HIP_MEM_D3D12_RESOURCE = 5,
};

struct win_sem_desc {
    unsigned int type;
    unsigned int pad;
    union {
        int fd;
        struct {
            void *handle;
            const void *name;
        } win32;
    } handle;
    unsigned int flags;
    unsigned int reserved[16];
};

typedef int (*wine_h2fd_t)(void *handle, unsigned int access, int *unix_fd, unsigned int *options);

static wine_h2fd_t resolve_h2fd(void) {
    static wine_h2fd_t fn;
    void *h;
    if (fn)
        return fn;
    fn = (wine_h2fd_t)dlsym(RTLD_DEFAULT, "wine_server_handle_to_fd");
    if (fn)
        return fn;
    h = dlopen("ntdll.so", RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
    if (!h)
        h = dlopen("ntdll.so", RTLD_NOW | RTLD_GLOBAL);
    if (h)
        fn = (wine_h2fd_t)dlsym(h, "wine_server_handle_to_fd");
    blog("resolve h2fd=%p ntdll=%p", (void *)fn, h);
    return fn;
}

typedef unsigned int d3dkmt_handle_t;
typedef d3dkmt_handle_t (*d3dkmt_open_resource_t)(d3dkmt_handle_t, void *, d3dkmt_handle_t *,
                                                  d3dkmt_handle_t *);
typedef int (*d3dkmt_object_get_fd_t)(d3dkmt_handle_t);
typedef int (*d3dkmt_destroy_resource_t)(d3dkmt_handle_t);
typedef d3dkmt_handle_t (*d3dkmt_open_sync_t)(d3dkmt_handle_t, void *);
typedef int (*d3dkmt_destroy_sync_t)(d3dkmt_handle_t);

static void *elf_local_sym(void *handle, const char *name) {
    struct link_map *lm = NULL;
    struct stat st;
    const ElfW(Ehdr) * eh;
    const ElfW(Shdr) * sh, *symtab = NULL, *strtab = NULL;
    const ElfW(Sym) * sym;
    const char *strs;
    void *map;
    void *found = NULL;
    int fd, i, n;
    if (!handle || !name)
        return NULL;
    if (dlinfo(handle, RTLD_DI_LINKMAP, &lm) != 0 || !lm || !lm->l_name || !lm->l_name[0])
        return NULL;
    fd = open(lm->l_name, O_RDONLY);
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(ElfW(Ehdr))) {
        close(fd);
        return NULL;
    }
    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return NULL;
    eh = (const ElfW(Ehdr) *)map;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0)
        goto out;
    sh = (const ElfW(Shdr) *)((const char *)map + eh->e_shoff);
    for (i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {
            symtab = &sh[i];
            if (sh[i].sh_link < (unsigned)eh->e_shnum)
                strtab = &sh[sh[i].sh_link];
            break;
        }
    }
    if (!symtab || !strtab || !symtab->sh_entsize)
        goto out;
    strs = (const char *)map + strtab->sh_offset;
    sym = (const ElfW(Sym) *)((const char *)map + symtab->sh_offset);
    n = (int)(symtab->sh_size / symtab->sh_entsize);
    for (i = 0; i < n; i++) {
        if (!sym[i].st_name || ELF64_ST_TYPE(sym[i].st_info) != STT_FUNC)
            continue;
        if (strcmp(strs + sym[i].st_name, name) == 0) {
            found = (char *)lm->l_addr + sym[i].st_value;
            break;
        }
    }
out:
    munmap(map, (size_t)st.st_size);
    return found;
}

static int d3dkmt_handle_to_fd(void *nt_handle, int is_sync) {
    static int resolved;
    static d3dkmt_open_resource_t open_res;
    static d3dkmt_open_sync_t open_sync;
    static d3dkmt_object_get_fd_t get_fd;
    static d3dkmt_destroy_resource_t destroy_res;
    static d3dkmt_destroy_sync_t destroy_sync;
    void *h;
    if (!resolved) {
        resolved = 1;
        h = dlopen("win32u.so", RTLD_NOW | RTLD_NOLOAD);
        if (!h)
            h = dlopen("win32u.so", RTLD_NOW);
        open_res = (d3dkmt_open_resource_t)elf_local_sym(h, "d3dkmt_open_resource");
        open_sync = (d3dkmt_open_sync_t)elf_local_sym(h, "d3dkmt_open_sync");
        get_fd = (d3dkmt_object_get_fd_t)elf_local_sym(h, "d3dkmt_object_get_fd");
        destroy_res = (d3dkmt_destroy_resource_t)elf_local_sym(h, "d3dkmt_destroy_resource");
        destroy_sync = (d3dkmt_destroy_sync_t)elf_local_sym(h, "d3dkmt_destroy_sync");
        blog("d3dkmt win32u=%p open_res=%p open_sync=%p get_fd=%p", h, (void *)open_res,
             (void *)open_sync, (void *)get_fd);
    }
    if (!get_fd)
        return -1;
    if (is_sync) {
        d3dkmt_handle_t local;
        int fd;
        if (!open_sync)
            return -1;
        local = open_sync(0, nt_handle);
        if (!local)
            return -1;
        fd = get_fd(local);
        if (destroy_sync)
            destroy_sync(local);
        return fd;
    }
    {
        d3dkmt_handle_t local, mutex = 0, sync = 0;
        int fd;
        if (!open_res)
            return -1;
        local = open_res(0, nt_handle, &mutex, &sync);
        if (!local)
            return -1;
        fd = get_fd(local);
        if (destroy_res)
            destroy_res(local);
        return fd;
    }
}

static int nt_to_fd(void *nt_handle, int is_sync) {
    wine_h2fd_t h2fd = resolve_h2fd();
    int fd = -1;
    if (h2fd) {
        int st;
        unsigned int options = 0;
        st = h2fd(nt_handle, 0x80000000u | 0x40000000u, &fd, &options);
        if ((st != 0 || fd < 0))
            st = h2fd(nt_handle, 0x10000000u, &fd, &options);
        if ((st != 0 || fd < 0))
            fd = -1;
    }
    if (fd < 0)
        fd = d3dkmt_handle_to_fd(nt_handle, is_sync);
    return fd;
}

/* Passthrough stubs (lazy HIP open). */
#define STUB0(ret, name) \
    static ret stub_##name(void) { \
        if (ensure_hip() || !g_bridge.p_##name) \
            return (ret)3; \
        return g_bridge.p_##name(); \
    }
#define STUB1(ret, name, T1) \
    static ret stub_##name(T1 a) { \
        if (ensure_hip() || !g_bridge.p_##name) \
            return (ret)3; \
        return g_bridge.p_##name(a); \
    }
#define STUB2(ret, name, T1, T2) \
    static ret stub_##name(T1 a, T2 b) { \
        if (ensure_hip() || !g_bridge.p_##name) \
            return (ret)3; \
        return g_bridge.p_##name(a, b); \
    }
#define STUB3(ret, name, T1, T2, T3) \
    static ret stub_##name(T1 a, T2 b, T3 c) { \
        if (ensure_hip() || !g_bridge.p_##name) \
            return (ret)3; \
        return g_bridge.p_##name(a, b, c); \
    }
#define STUB4(ret, name, T1, T2, T3, T4) \
    static ret stub_##name(T1 a, T2 b, T3 c, T4 d) { \
        if (ensure_hip() || !g_bridge.p_##name) \
            return (ret)3; \
        return g_bridge.p_##name(a, b, c, d); \
    }
#define STUB5(ret, name, T1, T2, T3, T4, T5) \
    static ret stub_##name(T1 a, T2 b, T3 c, T4 d, T5 e) { \
        if (ensure_hip() || !g_bridge.p_##name) \
            return (ret)3; \
        return g_bridge.p_##name(a, b, c, d, e); \
    }

STUB1(int, hipInit, unsigned)
STUB1(int, hipRuntimeGetVersion, int *)
STUB1(int, hipDriverGetVersion, int *)
STUB1(int, hipGetDeviceCount, int *)
STUB1(int, hipSetDevice, int)
STUB2(int, hipGetDevicePropertiesR0600, void *, int)
STUB3(int, hipDeviceGetName, char *, int, int)
STUB2(int, hipMemGetInfo, unsigned long *, unsigned long *)
STUB2(int, hipMalloc, void **, unsigned long)
STUB1(int, hipFree, void *)
STUB3(int, hipHostMalloc, void **, unsigned long, unsigned)
STUB4(int, hipMemcpy, void *, const void *, unsigned long, int)
STUB5(int, hipMemcpyAsync, void *, const void *, unsigned long, int, void *)
STUB4(int, hipMemsetAsync, void *, int, unsigned long, void *)
STUB1(int, hipStreamCreate, void **)
STUB1(int, hipStreamSynchronize, void *)
STUB1(int, hipStreamDestroy, void *)
STUB0(int, hipDeviceSynchronize)
STUB1(int, hipEventCreate, void **)
STUB2(int, hipEventRecord, void *, void *)
STUB3(int, hipEventElapsedTime, float *, void *, void *)
STUB1(int, hipEventDestroy, void *)
STUB1(int, hipEventSynchronize, void *)
STUB1(int, hipDestroyExternalMemory, void *)
STUB1(int, hipDestroyExternalSemaphore, void *)
STUB2(int, hipModuleLoadData, void **, const void *)
STUB2(int, hipModuleLoad, void **, const char *)
STUB3(int, hipModuleGetFunction, void **, void *, const char *)
STUB1(int, hipModuleUnload, void *)

static int stub_hipModuleLaunchKernel(void *f, unsigned bx, unsigned by, unsigned bz, unsigned tx,
                                      unsigned ty, unsigned tz, unsigned sh, void *stream, void **args,
                                      void **extra) {
    if (ensure_hip() || !g_bridge.p_hipModuleLaunchKernel)
        return 3;
    return g_bridge.p_hipModuleLaunchKernel(f, bx, by, bz, tx, ty, tz, sh, stream, args, extra);
}

static int stub_hipImportExternalMemory(void **out, const void *desc) {
    const struct win_extmem_desc *in;
    struct win_extmem_desc fd_desc;
    int fd;
    if (ensure_hip() || !g_bridge.p_hipImportExternalMemory)
        return 1;
    if (!out || !desc)
        return 1;
    in = (const struct win_extmem_desc *)desc;
    if (in->type == HIP_MEM_OPAQUE_FD)
        return g_bridge.p_hipImportExternalMemory(out, desc);
    if (!in->handle.win32.handle) {
        blog("import: null win32 handle type=%u", in->type);
        return 1;
    }
    fd = nt_to_fd(in->handle.win32.handle, 0);
    if (fd < 0) {
        blog("import: no dma-buf fd for handle=%p type=%u", in->handle.win32.handle, in->type);
        return 1;
    }
    memset(&fd_desc, 0, sizeof(fd_desc));
    fd_desc.type = HIP_MEM_OPAQUE_FD;
    fd_desc.handle.fd = fd;
    fd_desc.size = in->size;
    fd_desc.flags = in->flags;
    {
        int err = g_bridge.p_hipImportExternalMemory(out, &fd_desc);
        /* HIP duplicates the fd on success; ours can be closed either way. */
        close(fd);
        if (err)
            blog("OpaqueFd import err=%d size=%llu", err, (unsigned long long)in->size);
        return err;
    }
}

static int stub_hipExternalMemoryGetMappedBuffer(void **ptr, void *mem, const void *desc) {
    if (ensure_hip() || !g_bridge.p_hipExternalMemoryGetMappedBuffer)
        return 1;
    return g_bridge.p_hipExternalMemoryGetMappedBuffer(ptr, mem, desc);
}

static int stub_hipImportExternalSemaphore(void **out, const void *desc) {
    const struct win_sem_desc *in;
    struct win_sem_desc fd_desc;
    int fd;
    if (ensure_hip() || !g_bridge.p_hipImportExternalSemaphore)
        return 1;
    if (!out || !desc)
        return 1;
    in = (const struct win_sem_desc *)desc;
    if (in->type == HIP_MEM_OPAQUE_FD)
        return g_bridge.p_hipImportExternalSemaphore(out, desc);
    if (!in->handle.win32.handle) {
        blog("sem import: null win32 handle");
        return 1;
    }
    fd = nt_to_fd(in->handle.win32.handle, 1);
    if (fd < 0) {
        blog("sem import: no fd for handle=%p", in->handle.win32.handle);
        return 1;
    }
    memset(&fd_desc, 0, sizeof(fd_desc));
    fd_desc.type = HIP_MEM_OPAQUE_FD;
    fd_desc.handle.fd = fd;
    fd_desc.flags = in->flags;
    {
        int err = g_bridge.p_hipImportExternalSemaphore(out, &fd_desc);
        close(fd);
        if (err)
            blog("sem OpaqueFd import err=%d", err);
        return err;
    }
}

static int stub_hipSignalExternalSemaphoresAsync(const void *sems, const void *params, unsigned n,
                                                 void *stream) {
    if (ensure_hip() || !g_bridge.p_hipSignalExternalSemaphoresAsync)
        return 3;
    return g_bridge.p_hipSignalExternalSemaphoresAsync(sems, params, n, stream);
}

static int stub_hipWaitExternalSemaphoresAsync(const void *sems, const void *params, unsigned n,
                                               void *stream) {
    if (ensure_hip() || !g_bridge.p_hipWaitExternalSemaphoresAsync)
        return 3;
    return g_bridge.p_hipWaitExternalSemaphoresAsync(sems, params, n, stream);
}

static const char *stub_hipGetErrorName(int e) {
    if (ensure_hip() || !g_bridge.p_hipGetErrorName)
        return "hip bridge not ready";
    return g_bridge.p_hipGetErrorName(e);
}

static NeuralAmdHipBridge g_public;

__attribute__((constructor)) static void neural_amd_hip_init(void) {
    memset(&g_bridge, 0, sizeof(g_bridge));
    memset(&g_public, 0, sizeof(g_public));
    g_public.magic = NEURAL_AMD_HIP_MAGIC;
    g_public.p_hipInit = stub_hipInit;
    g_public.p_hipRuntimeGetVersion = stub_hipRuntimeGetVersion;
    g_public.p_hipDriverGetVersion = stub_hipDriverGetVersion;
    g_public.p_hipGetDeviceCount = stub_hipGetDeviceCount;
    g_public.p_hipSetDevice = stub_hipSetDevice;
    g_public.p_hipGetDevicePropertiesR0600 = stub_hipGetDevicePropertiesR0600;
    g_public.p_hipDeviceGetName = stub_hipDeviceGetName;
    g_public.p_hipMemGetInfo = stub_hipMemGetInfo;
    g_public.p_hipMalloc = stub_hipMalloc;
    g_public.p_hipFree = stub_hipFree;
    g_public.p_hipHostMalloc = stub_hipHostMalloc;
    g_public.p_hipMemcpy = stub_hipMemcpy;
    g_public.p_hipMemcpyAsync = stub_hipMemcpyAsync;
    g_public.p_hipMemsetAsync = stub_hipMemsetAsync;
    g_public.p_hipStreamCreate = stub_hipStreamCreate;
    g_public.p_hipStreamSynchronize = stub_hipStreamSynchronize;
    g_public.p_hipStreamDestroy = stub_hipStreamDestroy;
    g_public.p_hipDeviceSynchronize = stub_hipDeviceSynchronize;
    g_public.p_hipEventCreate = stub_hipEventCreate;
    g_public.p_hipEventRecord = stub_hipEventRecord;
    g_public.p_hipEventElapsedTime = stub_hipEventElapsedTime;
    g_public.p_hipEventDestroy = stub_hipEventDestroy;
    g_public.p_hipEventSynchronize = stub_hipEventSynchronize;
    g_public.p_hipImportExternalMemory = stub_hipImportExternalMemory;
    g_public.p_hipExternalMemoryGetMappedBuffer = stub_hipExternalMemoryGetMappedBuffer;
    g_public.p_hipDestroyExternalMemory = stub_hipDestroyExternalMemory;
    g_public.p_hipImportExternalSemaphore = stub_hipImportExternalSemaphore;
    g_public.p_hipSignalExternalSemaphoresAsync = stub_hipSignalExternalSemaphoresAsync;
    g_public.p_hipWaitExternalSemaphoresAsync = stub_hipWaitExternalSemaphoresAsync;
    g_public.p_hipDestroyExternalSemaphore = stub_hipDestroyExternalSemaphore;
    g_public.p_hipModuleLoadData = stub_hipModuleLoadData;
    g_public.p_hipModuleLoad = stub_hipModuleLoad;
    g_public.p_hipModuleGetFunction = stub_hipModuleGetFunction;
    g_public.p_hipModuleLaunchKernel = stub_hipModuleLaunchKernel;
    g_public.p_hipModuleUnload = stub_hipModuleUnload;
    g_public.p_hipGetErrorName = stub_hipGetErrorName;

    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%p", (void *)&g_public);
        setenv(NEURAL_AMD_HIP_ENV, buf, 1);
        /* Legacy alias used by earlier prototypes. */
        setenv("DLSSNR_HIP_BRIDGE", buf, 1);
    }
    blog("preload table=%p (HIP not opened yet)", (void *)&g_public);
}
