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
generation. PointInstancer and nested HdInstancer transforms are expanded from
public Hydra APIs, including per-instance translation, rotation, scale, and
matrix primvars. MaterialX modules are compiled and retained by Hydra materials;
the GPU path integrator evaluates constant and image-driven
`standard_surface`/Preview Surface base color, metalness, roughness, emission,
opacity, tangent-space normals, transmission, and the Standard Surface
subsurface inputs. Hydra face-varying/indexed UV seams are preserved, Hio
decodes source images, and a partially-bound Vulkan descriptor array provides
sRGB-aware texture sampling. Authored mesh `normals` and `primvars:normals`
follow Hydra's precedence rules and support indexed constant, uniform, vertex,
varying, and face-varying interpolation. Normals are transformed correctly for
non-uniformly scaled meshes and instances before GPU interpolation.

The supported graph subset follows direct image connections (including a
`normalmap` node) into the surface. General MaterialX procedural graphs, UDIMs,
texture transforms, coat, sheen, a random-walk BSSRDF, and direct callable
integration of generated MaterialX functions remain future work. The current
subsurface implementation is a realtime diffusion approximation. Generated
Vulkan raster stages are compiled and cached, but cannot be invoked directly
from the compute path tracer; supported inputs are lowered into hdCodex's
compute material ABI.

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
