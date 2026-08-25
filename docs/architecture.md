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

Static scene and accumulation buffers reside in device-local memory and are
populated/read through staging buffers. A reusable command buffer and fence avoid
per-sample command allocation and whole-queue idle waits. Scenes without alpha
cutouts mark their geometry and ray queries opaque, bypassing opacity candidate
evaluation for primary, continuation, and shadow rays. Materials also skip
inactive transmission, subsurface, and coat work.

The production integrator traces up to eight bounces (with a twelve-bounce GPU
limit) and evaluates Lambert diffuse plus dielectric, metal, and layered-coat
GGX reflection. It transports correlated sampled wavelengths, reconstructs
authored RGB inputs into spectra, and integrates CIE XYZ only at the camera. It
uploads as many as 64
visible UsdLux DomeLight and RectLight records with their color temperature,
intensity/exposure, diffuse/specular controls, shaping, texture, transform, and
shadow parameters. Dome textures are evaluated for background rays and sampled
uniformly for direct lighting; rectangles are sampled in area and converted to
solid-angle measure. One authored light is selected per shading event and its
selection probability is included in the estimator. RectLight `normalize`
divides emitted radiance by world-space area as required by UsdLux.

If the stage contains no supported authored lights, the shader falls back to a
neutral analytic sky and a shadow-casting sun at 75° elevation with an oblique
azimuth. This keeps `usdrecord --disableCameraLight` useful for unlit assets
without mixing fallback illumination into authored-light scenes. The Y-up or
Z-up fallback axis is inferred once from the initial camera; the sky and sun
then remain fixed in world space as the camera moves. The renderer
evaluates eight progressive samples per Vulkan dispatch and converges at 128
spatial samples by default. Each spatial path is evaluated as a correlated three-wavelength
packet with deterministic spectral stratification.
Scene or camera changes reset accumulation. Geometry is flattened into one
world-space BLAS for this first functional path; per-mesh BLAS caching and TLAS
instance updates remain a performance optimization.

While the camera changes, the render pass traces at half width and height with a
two-bounce limit, then nearest-upscales the preview into the Hydra color AOV.
Once the camera is stationary it restarts at full resolution and the normal
eight-bounce limit, so interactive quality does not alter the converged result.

Hydra instancing is flattened at scene synchronization time as well. The
renderer-owned `HdCodexInstancer` composes the instancer transform with indexed
translation, quaternion rotation, scale, and matrix primvars, then recursively
expands parent instancers. This supports PointInstancer and nested HdInstancer
chains without depending on `hdEmbree` implementation symbols. Moving these
instances into reusable BLAS plus native TLAS instances is a future performance
optimization.

The delegate advertises Hydra `extComputation` sprims and evaluates computed
primvars through `HdExtComputationUtils` during mesh synchronization. This lets
UsdImaging's public UsdSkel adapter provide CPU-skinned points and normals at the
current time code. Animated deformation dirties the mesh, rebuilds the flattened
geometry and BLAS, and resets accumulation; cached deforming BLAS updates remain
a future performance optimization.

Hydra face-set material subsets are lowered to per-triangle material IDs using
the coarse-face index encoded by `HdMeshUtil` during triangulation. The GPU
already shades through a triangle-material buffer, so subset support does not
duplicate vertices or split deforming meshes.

UsdImaging resolves the schema `normals` attribute and `primvars:normals` into
the Hydra `normals` primvar, with the primvar taking precedence. The mesh adapter
accepts indexed or unindexed float, double, and half arrays at constant,
uniform, vertex, varying, or face-varying interpolation. It triangulates them in
face-corner order and applies the inverse-transpose of each mesh/instance world
transform. The compute shader barycentrically interpolates these shading
normals, keeps the geometric triangle normal for facing and ray offsets, and
then applies any tangent-space material normal map. Missing or invalid authored
normals are generated as smooth vertex normals using public `Hd_VertexAdjacency`
and `Hd_SmoothNormals` APIs. Degenerate triangles retain the geometric normal as
the final shader fallback.

## MaterialX compilation

The default pipeline is:

1. Build a MaterialX document from the composed Hydra network.
2. Generate Vulkan GLSL using MaterialX's Vulkan shader generator.
3. Compile the generated raster stages to SPIR-V with glslang and cache them
   under a SHA-256 key containing source, generator/compiler versions, target,
   and ABI.
4. Separately lower supported OpenPBR, Standard Surface, and Preview Surface
   parameters and connected image nodes into the path tracer's compute material
   ABI.
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

The GPU BSDF binding supports constant and image-driven OpenPBR,
`standard_surface`, and Preview Surface base color, metalness, specular
roughness, emission, opacity, tangent-space normals, transmission, dielectric
specular, and coat. Indexed and face-varying UVs are triangulated in face-corner
order. Images are normalized to RGBA8, uploaded as sRGB or raw Vulkan images,
and accessed through a partially-bound descriptor array currently capped at
256 textures.
Opacity participates in primary and shadow ray-query candidate confirmation.
Transmission supports spectral color, wavelength-dependent Cauchy IOR from
OpenPBR dispersion controls, Fresnel refraction, thin-walled surfaces, Beer-law
absorption, and homogeneous interior scattering.
Specular and coat use energy-partitioned GGX lobes for direct and indirect
lighting. Standard Surface and OpenPBR subsurface controls drive a bounded
spectral random walk using authored mean-free-path radius, color, scale, and
anisotropy. A wrapped direct-light term remains as a low-variance surface
contribution.

Meshes with no material binding retain the linear `displayColor` supplied by
Hydra. The scene builder deduplicates these colors and assigns a default
material with Lambert diffuse, roughness 0.5, and the 4% dielectric reflection
of IOR 1.5. This fallback is restricted to empty material IDs; a bound but
unsupported material continues through the normal material fallback path.

Hydra materials retain the complete generated MaterialX raster-stage modules.
Vulkan does not make a fragment-stage function directly callable from a compute
shader, so hdCodex separately lowers the supported graph subset into its compute
ABI. Arbitrary procedural nodes, UDIMs, texture transforms, sheen, an unbounded
multi-scatter BSSRDF,
and a general MaterialX-to-path-tracer callable ABI are not implemented.

## Subdivision surfaces

Subdivision meshes are currently rendered from their coarse polygon topology.
The planned implementation uses only public `HdMeshTopology`, `PxOsd`, and
OpenSubdiv APIs, with cached uniform refinement first and GPU stencil evaluation
later. See [Subdivision plan](subdivision-plan.md) for the data model, correctness
requirements, tests, and performance phases.

## Threading

Scene mutation and publication are mutex-protected. Hydra executes GPU work on
the render-pass thread. The backend retains a command buffer, evaluates a batch
of progressive samples per submission, waits on a scoped fence, copies the
device-local accumulation into a host-visible staging buffer once per batch, and
bulk-copies RGBA32F into the Hydra color AOV. A multi-buffered one-frame-latency
readback or direct graphics-API interop would be required to remove the final
synchronous CPU handoff. Per-mesh BLAS reuse, native TLAS instances, GPU timestamp
instrumentation, ray-differential texture LODs, and overlapped readback remain
future performance work.
