# hdCodex

`hdCodex` is a standalone OpenUSD Hydra render delegate backed by a GPU path
tracer. The primary backend uses Vulkan ray queries so hardware ray traversal is
available across NVIDIA, AMD, and Intel devices. MaterialX graphs are generated
to Vulkan GLSL, compiled to SPIR-V at runtime, and cached by content and compiler
configuration. SPIR-V is performance-optimized by default.

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
opacity, tangent-space normals, transmission, dielectric/metal GGX specular,
Standard Surface coat, and the Standard Surface subsurface inputs. Hydra
face-varying/indexed UV seams are preserved, Hio
decodes source images, and a partially-bound Vulkan descriptor array provides
sRGB-aware texture sampling. Authored mesh `normals` and `primvars:normals`
follow Hydra's precedence rules and support indexed constant, uniform, vertex,
varying, and face-varying interpolation. Normals are transformed correctly for
non-uniformly scaled meshes and instances before GPU interpolation. When a mesh
does not author normals, the adapter generates smooth vertex normals from Hydra's
public adjacency and smooth-normal utilities.

UsdLux `DomeLight` and `RectLight` prims now drive GPU lighting directly. Dome
lights support HDR textures and the standard lat-long, mirrored-ball, angular,
and vertical-cross layouts. Rectangle lights support textured emission,
world-space area normalization, shaping controls, diffuse/specular multipliers,
and colored distance-limited shadows. When a stage has no supported authored
lights, hdCodex retains a neutral analytic sky and an oblique default sun at 75°
elevation so command-line renders remain usable with camera lighting disabled.

Meshes without a material binding use their Hydra `displayColor` as a linear
base color. These colors are deduplicated into default GPU materials with
Lambert diffuse and a small rough dielectric specular lobe, preserving useful
asset-authored viewport colors without treating unsupported bound materials as
unbound.

Camera motion uses a low-latency preview at half resolution and two bounces.
When motion stops, the render pass immediately returns to full-resolution,
five-bounce progressive accumulation. Opaque scenes use Vulkan's opaque
ray-query path, while scenes containing actual alpha cutouts retain candidate
opacity evaluation.

The supported graph subset follows direct image connections (including a
`normalmap` node) into the surface. General MaterialX procedural graphs, UDIMs,
texture transforms, sheen, a random-walk BSSRDF, and direct callable
integration of generated MaterialX functions remain future work. The current
subsurface implementation is a realtime diffusion approximation. Generated
Vulkan raster stages are compiled and cached, but cannot be invoked directly
from the compute path tracer; supported inputs are lowered into hdCodex's
compute material ABI.

See [Architecture](docs/architecture.md) for boundaries and implementation
phases, [Subdivision plan](docs/subdivision-plan.md) for the staged OpenSubdiv
integration, [Gallery](gallery.md) for versioned reference renders and timings,
and [Building](docs/building.md) for local dependency setup.

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
render_codex.bat --imageWidth 512 --camera renderCam gallery\chess_board.usda gallery\chess_board.jpg
launch_codex.bat
```

`render_test.bat` defaults to the supplied OpenChessSet asset and accepts an
optional scene and output path. All USD-facing scripts call
`setup_usd_env.bat`; none use Houdini libraries.
