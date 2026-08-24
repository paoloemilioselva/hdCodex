# Architecture

## Goals

- A loadable Hydra render delegate that uses only public OpenUSD headers.
- Progressive, interruptible GPU path tracing with hardware acceleration.
- MaterialX code generation and runtime compilation with deterministic caching.
- No process-global renderer singleton: each `HdRenderIndex` owns its delegate,
  render parameter, scene, device, and render thread.
- Dependency injection at the GPU boundary so scene synchronization and cache
  behavior can be tested without a GPU or OpenUSD runtime.

## Namespace and ABI boundary

Hydra-discoverable adapter classes are named `pxr::HdCodex*` and use a dedicated
`HDCODEX_API` export macro. Renderer implementation classes use `hdcodex::*`.
The project never includes private OpenUSD headers or references symbols from
`hdEmbree`; this is the key constraint that allows an out-of-tree build.

## Data flow

```text
Hydra scene delegates
        |
        v
HdCodex mesh/material/camera adapters -- Sync() --> hdcodex::Scene
                                                    |
MaterialX network --> source generator --> cache -->| compiled stage modules
       |                                            |
       +--> surface/image lowering + Hio textures ->| compute material ABI
                                                    v
                                           VulkanPathTracer
                                             BLAS / TLAS
                                                    |
                                       progressive accumulation
                                                    v
                                      HdCodexRenderBuffer
```

Hydra `Sync()` calls only update immutable scene snapshots and dirty-version
counters. `CommitResources()` publishes a coherent snapshot. The render pass
starts or restarts progressive rendering when the snapshot, camera, AOV layout,
or render settings version changes.

## GPU backend

The primary backend is Vulkan 1.3 plus:

- `VK_KHR_acceleration_structure`
- `VK_KHR_ray_query`
- `VK_KHR_buffer_device_address`
- `VK_KHR_deferred_host_operations`
- `VK_EXT_descriptor_indexing`

A compute shader controls the path loop and invokes ray queries for traversal.
This keeps the renderer's integrator and material dispatch explicit while using
vendor RT hardware. The current implementation flattens visible world-space
meshes into one BLAS and builds a one-instance TLAS whenever the published scene
changes. Accumulation resets for scene, camera, or output-size changes.

The initial integrator traces up to five diffuse bounces, samples an analytic
sky, performs explicit sun shadow queries, accumulates one stochastic sample
per Hydra execute, and converges at 64 samples. Scene or camera changes reset
accumulation. Geometry is flattened into one world-space BLAS for this first
functional path; per-mesh BLAS caching and TLAS instance updates remain a
performance optimization.

Hydra instancing is flattened at scene synchronization time as well. The
renderer-owned `HdCodexInstancer` composes the instancer transform with indexed
translation, quaternion rotation, scale, and matrix primvars, then recursively
expands parent instancers. This supports PointInstancer and nested HdInstancer
chains without depending on `hdEmbree` implementation symbols. Moving these
instances into reusable BLAS plus native TLAS instances is a future performance
optimization.

UsdImaging resolves the schema `normals` attribute and `primvars:normals` into
the Hydra `normals` primvar, with the primvar taking precedence. The mesh adapter
accepts indexed or unindexed float, double, and half arrays at constant,
uniform, vertex, varying, or face-varying interpolation. It triangulates them in
face-corner order and applies the inverse-transpose of each mesh/instance world
transform. The compute shader barycentrically interpolates these shading
normals, keeps the geometric triangle normal for facing and ray offsets, and
then applies any tangent-space material normal map. Missing or invalid authored
normals fall back to faceted geometric normals.

## MaterialX compilation

The default pipeline is:

1. Build a MaterialX document from the composed Hydra network.
2. Generate Vulkan GLSL using MaterialX's Vulkan shader generator.
3. Compile the generated raster stages to SPIR-V with glslang and cache them
   under a SHA-256 key containing source, generator/compiler versions, target,
   and ABI.
4. Separately lower supported Standard/Preview Surface parameters and connected
   image nodes into the path tracer's compute material ABI.
5. Decode referenced images through Hio and upload them to descriptor-indexed
   Vulkan images with sRGB/raw formats selected per input role.

glslang is embedded and pinned to the same Khronos Vulkan SDK release as the
headers and loader. Compilation targets Vulkan 1.3 / SPIR-V 1.6; stage, entry
point, optimization mode, and material ABI are all part of the cache key.

OpenUSD 26.03 contains MaterialX 1.39.3. Its Vulkan generator emits individually
located stage connector variables but retains the desktop-GLSL `vd.` instance
prefix in expressions. The compatibility adapter removes only this stale
prefix before glslang validation. It can be deleted when the standalone OpenUSD
distribution moves to a MaterialX version with the corrected generator.

The GPU BSDF binding supports constant and image-driven `standard_surface` and
Preview Surface base color, metalness, specular roughness, emission, opacity,
tangent-space normals, and transmission. Indexed and face-varying UVs are
triangulated in face-corner order. Images are normalized to RGBA8, uploaded as
sRGB or raw Vulkan images, and accessed through a partially-bound descriptor
array currently capped at 256 textures. Opacity participates in primary and
shadow ray-query candidate confirmation. Transmission supports color,
IOR/Fresnel refraction, and thin-walled surfaces. Standard Surface subsurface
weight, color, radius, and scale are lowered to a realtime attenuation and
wrapped-diffuse approximation; this is not yet a random-walk BSSRDF.

Hydra materials retain the complete generated MaterialX raster-stage modules.
Vulkan does not make a fragment-stage function directly callable from a compute
shader, so hdCodex separately lowers the supported graph subset into its compute
ABI. Arbitrary procedural nodes, UDIMs, texture transforms, coat, sheen, a full
BSSRDF, and a general MaterialX-to-path-tracer callable ABI are not implemented.

## Threading

Scene mutation and publication are mutex-protected. Hydra executes GPU work on
the render-pass thread, and the current backend waits for each compute
submission before copying its host-visible color buffer. Persistent asynchronous
command buffers, GPU-only accumulation, per-mesh BLAS reuse, and overlapped
readback are the next performance milestone.
