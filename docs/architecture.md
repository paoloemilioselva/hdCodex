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
MaterialX network --> source generator --> cache -->| GPU material programs
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
vendor RT hardware. A BLAS is cached per topology; the TLAS is rebuilt or updated
for transform/visibility changes. Accumulation resets only for changes that can
affect the image.

The initial integrator traces up to five diffuse bounces, samples an analytic
sky, performs explicit sun shadow queries, accumulates one stochastic sample
per Hydra execute, and converges at 64 samples. Scene or camera changes reset
accumulation. Geometry is flattened into one world-space BLAS for this first
functional path; per-mesh BLAS caching and TLAS instance updates remain a
performance optimization.

## MaterialX compilation

The default pipeline is:

1. Canonicalize the MaterialX document/network and generator options.
2. Generate Vulkan GLSL using MaterialX's Vulkan shader generator.
3. Adapt the generated surface function to the path tracer's BSDF ABI.
4. Compile GLSL to SPIR-V with glslang.
5. Cache source, reflection metadata, and SPIR-V under a SHA-256 key containing
   source, generator version, compiler version, target environment, and ABI.
6. Validate the cache header before loading and replace entries atomically.

glslang is embedded and pinned to the same Khronos Vulkan SDK release as the
headers and loader. Compilation targets Vulkan 1.3 / SPIR-V 1.6; stage, entry
point, optimization mode, and material ABI are all part of the cache key.

OpenUSD 26.03 contains MaterialX 1.39.3. Its Vulkan generator emits individually
located stage connector variables but retains the desktop-GLSL `vd.` instance
prefix in expressions. The compatibility adapter removes only this stale
prefix before glslang validation. It can be deleted when the standalone OpenUSD
distribution moves to a MaterialX version with the corrected generator.

The first GPU BSDF binding supports constant `standard_surface` and Preview
Surface base color, metalness, specular roughness, and emission values. Hydra
materials retain the complete generated MaterialX stage modules. UV primvars,
image descriptors, opacity/transmission, and callable integration of the
remaining generated closure interface are still in progress. Unsupported
closures produce a visible diagnostic material and a Hydra warning rather than
silently changing appearance.

## Threading

Scene mutation and publication are protected independently. GPU submission is
owned by one render worker. Shader compilation uses a bounded worker queue and
deduplicates concurrent requests for the same cache key. Stopping a delegate
joins the worker before GPU resources or render buffers are destroyed.
