# Proton HIP bridge (Lmxxf only)

Minimal Wine/Proton layer so the existing Lmxxf backend in `neural-amd-opti`
reaches Linux `libamdhip64.so.7`. No backend, launcher, deploy, vkd3d patch,
or Daniel logic is ported.

## Layout

* `hip_bridge_shared.h` — ABI between PE trampoline and Linux `.so`.
* `hip_bridge_linux.c` — Linux `.so`, `LD_PRELOAD`ed in the Wine process.
  Dlopens HIP, converts NT handles to dma-buf fds (`OpaqueFd`), forwards the
  rest. Publishes the table in `NEURAL_AMD_HIP_BRIDGE`.
* `amdhip64_7_trampoline.c` — PE `amdhip64_7.dll`. Forwards the `hip_api.h`
  Lmxxf surface to the bridge via `__wine_get_unix_env`. Not AMD's runtime.
* `Makefile` — `make` builds `neural_amd_hip_bridge.so`.

## Runtime wiring

```sh
make -C bridge/proton
# build amdhip64_7.dll with mingw/clang (see Makefile), or reuse existing PE

export DLSSNR_HIP_LIBRARY=/opt/rocm/lib/libamdhip64.so.7  # or LMXXF_HIP_LIBRARY
export LD_PRELOAD=/path/to/neural_amd_hip_bridge.so
# copy amdhip64_7.dll (trampoline) to prefix drive_c/windows/system32
# WINEDLLOVERRIDES: amdhip64_7=n (keep stock d3d12/dxgi from Proton)
```

Optional: `LMXXF_HIP_DEVICE=N` (or `DLSSNR_HIP_DEVICE=N`) pins the HIP index
when the D3D12 LUID has no R0600 match. Logs: `NEURAL_AMD_HIP_LOG`.

## Policy (code)

* `OptiScaler/dlssnr/amd/ProtonBridge.h` — `IsWine()` / `HipDeviceOverride()`.
* `OptiScaler/dlssnr/backend/Selector.cpp` — under Wine, `auto` prefers
  Lmxxf; explicit `daniel` resolves to `Off`.
* `OptiScaler/dlssnr/amd/AmdBridge.cpp` — fail-safe: never constructs
  `DanielBackend` under Wine.
* `third_party/lmxxf/Development/HIP/hip_d3d12_bridge.h` — `Create()` honors
  `LMXXF_HIP_DEVICE`/`DLSSNR_HIP_DEVICE`; stock LUID path unchanged.

Daniel stays Windows-only by design.
