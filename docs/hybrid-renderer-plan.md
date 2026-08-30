# Hybrid renderer plan

Temporal reconstruction, guide buffers, GPU-frame ownership, and optional
NVIDIA integration are tracked separately in the
[neural rendering and DLSS roadmap](neural-rendering-plan.md). Shared work such
as reusable BLAS/TLAS objects, stable IDs, motion, and GPU material evaluation
must satisfy both plans rather than being implemented twice.

## Decision

Keep the current spectral Vulkan path tracer as the production reference and do
not add a raster primary pass yet. The near-term performance work should first
remove scene-build and synchronization costs that benefit both pure path tracing
and any future hybrid renderer.

Vulkan ray queries are not an alternative to NVIDIA RTX hardware. They are a
portable Vulkan interface that can use the RT units present on RTX and equivalent
AMD/Intel GPUs. `VK_KHR_ray_tracing_pipeline` is a different Vulkan programming
model, not a guarantee of faster traversal. Retain ray queries until GPU timestamp
benchmarks show a material win for a pipeline backend on Sponza, OpenChessSet,
Glass, and the skinned Collective Project scene.

## Proposed hybrid pipeline

1. Rasterize primary visibility into depth, world position, geometric/shading
   normal, UV, motion, primitive/material ID, and spectral-material parameters.
2. Evaluate deterministic direct lighting in a deferred pass, with the same
   UsdLux and material conventions used by the path tracer.
3. Launch spectral ray-query paths only for indirect diffuse, glossy reflection,
   refraction, volume, and subsurface transport.
4. Reproject accumulated secondary lighting with motion/depth/normal rejection.
5. Composite primary emission/direct lighting and secondary transport into the
   same CIE XYZ sensor/output transform as the reference integrator.

## Required groundwork

- Per-mesh local-space vertex/index buffers and reusable BLAS objects.
- Native TLAS instances and transform-only TLAS updates.
- BLAS update/refit paths for UsdSkel deformation, with rebuild fallback when
  topology or buffer capacity changes.
- A shared material evaluation library usable by compute and raster shaders.
- A Vulkan graphics pipeline, render targets, synchronization, resize handling,
  and Hydra AOV mapping.
- Stable primitive/material IDs across Hydra updates.
- Motion vectors and history invalidation for camera, transform, deformation,
  material, light, and topology changes.
- Separate reference and hybrid render settings so correctness comparisons remain
  possible.

## Validation gates

1. The G-buffer must reproduce primary hit position, normals, UVs, material ID,
   and opacity decisions from ray queries.
2. Raster direct lighting must match the path-traced direct-light term within a
   documented tolerance.
3. Glass, dispersion, absorption, and subsurface paths must still begin at the
   physically correct interface and remain spectral.
4. Animated UsdSkel and PointInstancer scenes must not retain stale history.
5. The complete gallery must show no unexplained differences.
6. GPU timings must demonstrate a meaningful frame-time improvement after adding
   G-buffer, synchronization, memory, and history costs.

## Size of change

This is a substantial renderer architecture project rather than a shader toggle.
It adds a second primary-visibility pipeline, duplicates or shares material
evaluation across raster and compute stages, and introduces temporal state. It
should be developed behind a render setting after BLAS/TLAS reuse and asynchronous
presentation are complete, then promoted only if the gallery and timing gates
justify the added complexity.
