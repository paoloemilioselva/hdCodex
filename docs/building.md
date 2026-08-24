# Building

## Requirements

- CMake 3.25 or newer
- A C++20 compiler
- OpenUSD with imaging/Hydra enabled
- A Vulkan 1.3 driver with ray-query support for GPU rendering
- MaterialX and glslang for runtime material compilation

Texture decoding uses OpenUSD's public Hio API and installed image plugins, so
no Houdini image libraries or separately versioned image dependency is used.

CUDA is not required. Vulkan ray queries use RT hardware where the driver exposes
it and also make the backend portable beyond NVIDIA GPUs.

## Dependency modes

`HDCODEX_FETCH_DEPENDENCIES=ON` downloads pinned Vulkan-Headers, volk, Vulkan
Memory Allocator, and glslang sources beneath `_deps/`. Nothing is installed
globally. `HDCODEX_OPENUSD_ROOT` must identify a standalone OpenUSD
installation. This build intentionally never searches a Houdini installation.

The dependency-free core build is useful for cache and scene tests:

```powershell
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
```

Complete configuration using the standalone OpenUSD 26.03 distribution:

```powershell
cmake --preset dev `
  -DHDCODEX_OPENUSD_ROOT="C:/dev/usd-26.03" `
  -DHDCODEX_FETCH_DEPENDENCIES=ON
cmake --build --preset dev
```

Fetched upstream dependencies remain beneath `_deps/`; build and runtime
artifacts remain beneath `build/`. The generated plugin
resource tree can be added to `PXR_PLUGINPATH_NAME` after installation.

`setup_usd_env.bat` is the single environment used by `compile.bat`,
`launch_codex.bat`, `validate_usd.bat`, and `render_test.bat`. It selects the
standalone OpenUSD plugins, libraries, and a Python 3.12 runtime. Set
`HDCODEX_PYTHON` to a standalone runtime directory, or unpack one beneath
`_deps/python`; known standalone installations are used only as fallbacks.
MaterialX discovery remains owned by OpenUSD's `hdMtlx` configuration, avoiding
duplicate standard-library imports. No Houdini path is appended.
