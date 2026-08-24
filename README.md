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
loadable Hydra plugin, triangulated scene snapshots, Vulkan BLAS/TLAS builds, a
five-bounce progressive ray-query path integrator, Hydra color-AOV output, an
in-process GLSL-to-SPIR-V compiler, and cached MaterialX Vulkan shader
generation. MaterialX modules are compiled and retained by Hydra materials;
constant `standard_surface`/Preview Surface base color, metalness, roughness,
and emission values are bound into the GPU path integrator. UV primvars,
texture images, opacity, and the remaining closure inputs are the next material
milestone.

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

On this workstation the complete standalone flow is:

```bat
compile.bat
validate_usd.bat
render_test.bat
launch_codex.bat
```

`render_test.bat` defaults to the supplied OpenChessSet asset and accepts an
optional scene and output path. All USD-facing scripts call
`setup_usd_env.bat`; none use Houdini libraries.
