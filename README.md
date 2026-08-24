# hdCodex

`hdCodex` is a standalone OpenUSD Hydra render delegate backed by a GPU path
tracer. The primary backend uses Vulkan ray queries so hardware ray traversal is
available across NVIDIA, AMD, and Intel devices. MaterialX graphs are generated
to Vulkan GLSL, compiled to SPIR-V at runtime, and cached by content and compiler
configuration.

The project is intentionally not derived from `hdEmbree`. Hydra adapter classes
use public OpenUSD APIs in the `pxr` namespace, while all renderer implementation
types live in the separate `hdcodex` namespace. This keeps the plugin buildable
outside the OpenUSD source tree.

## Status

This repository is under active development. The current milestone provides a
loadable Hydra plugin, scene/AOV adapters, a hardware ray-query Vulkan device,
an in-process GLSL-to-SPIR-V compiler, and cached MaterialX Vulkan shader
generation. BLAS/TLAS construction and the progressive path integration pass
are the next renderer milestone; the current render pass only clears AOVs.

See [Architecture](docs/architecture.md) for boundaries and implementation
phases, and [Building](docs/building.md) for local dependency setup.

## Quick start

The dependency-free core and tests can be built immediately:

```powershell
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
```

For the complete delegate, configure an OpenUSD install and enable the optional
dependency bootstrap described in `docs/building.md`.
