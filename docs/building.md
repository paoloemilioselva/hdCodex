# Building

## Requirements

- CMake 3.25 or newer
- A C++20 compiler
- OpenUSD with imaging/Hydra enabled
- A Vulkan 1.2 driver with ray-query support for GPU rendering
- MaterialX and shaderc for runtime material compilation

CUDA is not required. Vulkan ray queries use RT hardware where the driver exposes
it and also make the backend portable beyond NVIDIA GPUs.

## Dependency modes

`HDCODEX_FETCH_DEPENDENCIES=ON` downloads source-buildable dependencies into the
build directory. Nothing is installed globally. OpenUSD may be supplied with
`HDCODEX_OPENUSD_ROOT`; the finder accepts both a standard OpenUSD installation
and a Houdini installation containing `toolkit/include` and
`custom/houdini/dsolib`.

The dependency-free core build is useful for cache and scene tests:

```powershell
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
```

Complete configuration using a Houdini OpenUSD distribution:

```powershell
cmake --preset dev `
  -DHDCODEX_OPENUSD_ROOT="C:/Program Files/Side Effects Software/Houdini 21.0.631" `
  -DHDCODEX_FETCH_DEPENDENCIES=ON
cmake --build --preset dev
```

Build and runtime artifacts remain beneath `build/`. The generated plugin
resource tree can be added to `PXR_PLUGINPATH_NAME` after installation.

