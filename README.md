# hdCodex

`hdCodex` is an out-of-tree OpenUSD Hydra render delegate with a fully spectral
GPU path tracer. It uses Vulkan ray queries for hardware-accelerated traversal
on NVIDIA, AMD, and Intel GPUs; it does not depend on NVIDIA-only RTX APIs or on
private OpenUSD/`hdEmbree` implementation details.

The production integrator transports sampled wavelengths from 380–780 nm.
Authored RGB values and textures are reconstructed into spectra at shading time,
lights are evaluated spectrally, dielectric IOR can vary by wavelength, and the
virtual sensor integrates CIE XYZ before converting to linear sRGB. RGB is an
asset-input and display-output format, not the path-throughput representation.

## Important features

### USD and Hydra scene support

- Loadable Hydra renderer plugin named **Codex GPU Path Tracer**.
- Public OpenUSD APIs only, with renderer code isolated in the `hdcodex`
  namespace.
- Polygon triangulation with indexed, vertex, varying, uniform, constant, and
  face-varying primvars.
- Correct Y-up and Z-up scene handling without modifying authored geometry.
- Authored normals, generated smooth normals, tangent-space normal maps, and
  correct inverse-transpose normal transforms under non-uniform scale.
- PointInstancer and nested Hydra instancing, including translation, quaternion
  rotation, scale, and matrix instance primvars.
- Hydra `extComputation` evaluation for animated UsdSkel-skinned points and
  normals.
- Hydra geometry subsets retained as per-triangle material assignments, including
  subsets on skinned meshes.
- Unbound meshes use Hydra `displayColor` with a default Lambert material and a
  small rough dielectric specular lobe.

### Spectral path tracing

- Correlated three-wavelength hero packets spanning the visible spectrum.
- Smooth non-negative RGB-to-spectrum reconstruction for USD colors and image
  textures.
- CIE XYZ sensor integration and linear-sRGB output.
- Eight-bounce production paths, two-bounce interactive previews, Russian
  roulette, progressive accumulation, and deterministic wavelength
  stratification.
- MaterialX Lambert, qualitative and energy-preserving Oren-Nayar, and Burley
  diffuse plus energy-partitioned GGX reflection for dielectrics, artistic
  metalness conductors, and layered coats.
- Fresnel reflection/refraction, thin-walled transmission, wavelength-dependent
  Cauchy IOR from OpenPBR dispersion scale and Abbe number, total internal
  reflection, and nested-interface paths.
- Beer-law spectral absorption from transmission color/depth and homogeneous
  interior scattering with Henyey–Greenstein anisotropy.
- A bounded spectral random walk for subsurface materials, driven by authored
  color, mean-free-path radius, scale, and anisotropy. This gives materials such
  as the BubbleGum shader ball actual subsurface transport rather than only a
  wrapped-diffuse tint.
- Opacity cutouts participate in primary, continuation, and shadow ray queries;
  fully opaque scenes use Vulkan's faster opaque traversal path.

### Materials and textures

- MaterialX surface NodeGraphs are expanded by MaterialX and compiled from
  primitive NodeDefs and generic graph combiners into the compute closure ABI.
  The renderer contains no OpenPBR or Standard Surface parameter extractor;
  supported lobes and textures survive unsupported auxiliary decorations.
  Texture adjustments outside the compact ABI currently retain their source
  image as a documented approximation; unsupported terminal closures reject.
  USD-native `Usd*` networks remain explicitly unsupported.
- Constant or image-driven base color, metalness, roughness, emission, opacity,
  normal, transmission, specular color/weight, coat, and subsurface controls.
- Constant or image-driven MaterialX rough-diffuse weight and roughness in
  direct and indirect transport.
- MaterialX anisotropic GGX roughness pairs and layered Conty-Kulla/Zeltner
  sheen primitives, including OpenPBR fuzz and Standard Surface sheen graphs.
- OpenPBR transmission depth, volume scattering, dispersion, and subsurface
  radius/anisotropy controls.
- Hio image decoding, linear/HDR floating-point light textures, sRGB-aware
  material textures, face-varying UV seams, and descriptor-indexed Vulkan image
  sampling.
- MaterialX Vulkan GLSL generation, runtime glslang compilation to SPIR-V, and a
  deterministic content/configuration cache. Generated raster modules, their
  reflected descriptor ABI, and an expanded MaterialX graph program are kept
  by Hydra materials alongside the expanded closure program and its compiled
  path-tracing ABI.

### Lighting

- UsdLux `DomeLight`, `RectLight`, `DiskLight`, `SphereLight`,
  `CylinderLight`, and `DistantLight` ingestion.
- HDR dome textures with lat-long, mirrored-ball, angular, and vertical-cross
  layouts.
- Power-heuristic MIS between authored-dome next-event samples and non-delta
  BSDF continuation paths. Lat-long and automatic-layout HDR domes use
  luminance/solid-angle texture importance sampling; other projections retain
  an unbiased uniform-direction fallback.
- Rectangle-light transforms, textured emission, area normalization, shaping,
  diffuse/specular multipliers, and colored distance-limited shadows.
- Geometry-aware disk, sphere, and cylinder area sampling plus angular-diameter
  distant-light sampling; distant angle changes shadow softness without
  changing authored energy.
- Stable world-space fallback sky plus a shadow-casting sun at 75° elevation
  when the stage has no supported authored lights. The fallback selects Y-up or
  Z-up once and does not rotate with the camera.

### Interaction and performance

- Camera motion switches to a half-resolution, two-bounce preview and returns to
  full resolution when motion stops.
- Eight progressive samples are evaluated per compute dispatch, removing most
  command submissions, fence waits, and full-image GPU readbacks.
- Production rendering defaults to 128 spatial samples, exposed as 16 visible
  eight-sample refinement updates. Hydra clients can change `samplesPerPixel`,
  `maxBounces`, and `samplesPerUpdate` without rebuilding the delegate.
- Static scene and accumulation data use device-local buffers; staging resources,
  command buffers, fences, descriptor sets, and shader-cache entries are reused.
- The checked-in 512 px Gold shader-ball benchmark dropped from 16.61 s to about
  6.6 s at the cleaner 128-sample default.

## Current architecture and limits

Hydra adapters publish immutable scene snapshots to `VulkanPathTracer`. The
current backend flattens visible world-space geometry into one BLAS and builds a
single-instance TLAS when the published scene changes. Camera-only changes reuse
the acceleration structures. Per-mesh BLAS caching, native TLAS instances,
deforming-BLAS refits, overlapped readback, ray differentials/texture LOD, UDIM
tiles beyond 1023, subdivision refinement, arbitrary MaterialX procedural
graphs, and more UsdLux light types remain future work.

Vulkan ray queries are the primary backend because they expose the same hardware
RT units through a portable API and keep traversal inside the compute integrator.
A `VK_KHR_ray_tracing_pipeline` backend should only replace it after an
apples-to-apples benchmark on representative gallery scenes. A deferred-raster
primary pass plus path-traced secondary transport is deliberately postponed; its
design and validation stages are recorded in the
[hybrid renderer plan](docs/hybrid-renderer-plan.md).

See [Architecture](docs/architecture.md) for implementation boundaries,
[Building](docs/building.md) for dependency setup,
[Shading development](docs/shading_development.md) for MaterialX decisions and
the live shading capability audit,
[Subdivision plan](docs/subdivision-plan.md) for planned OpenSubdiv integration,
and [Gallery](gallery.md) for versioned reference renders and timings.

## Build and test

The dependency-free core can be built with CMake presets:

```powershell
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
```

For the complete delegate, configure the standalone OpenUSD/Vulkan dependencies
described in `docs/building.md`. On the development workstation:

```bat
compile.bat
validate_usd.bat
render_test.bat
render_codex.bat --imageWidth 512 --camera renderCam gallery\chess_board.usda gallery\chess_board.jpg
launch_codex.bat
```

All USD-facing scripts call `setup_usd_env.bat`, including Python `pxr` tools.
The scripts do not use Houdini libraries. `render_codex.bat` accepts normal
`usdrecord` arguments and always selects the Codex delegate with camera-lighting
disabled so authored or fallback lighting is tested.

For command-line tools that do not expose Hydra settings directly, set the
equivalent environment variables before launching the render:

```bat
set HDCODEX_SAMPLES_PER_PIXEL=512
set HDCODEX_SAMPLES_PER_UPDATE=8
set HDCODEX_MAX_BOUNCES=10
render_codex.bat --imageWidth 1024 scene.usda output.exr
```

Valid sample targets are 1–4096, update batches are 1–64, and path depth is
1–12. Larger sample targets continue refining the same estimator; they do not
change materials, lighting, or spectral reconstruction.

## Regression gallery

The images in `gallery/` are checked-in visual baselines. They cover:

- Intel Sponza for a large textured architectural scene.
- OpenChessSet for instancing, HDR lighting, and many materials.
- StandardShaderBall Glass, Gold, and BubbleGum for spectral dielectric,
  conductor, texture, and subsurface behavior.
- Pixar KitchenSet for Z-up coordinates and per-mesh `displayColor` fallback.
- Collective Project 001 for UsdSkel deformation and mesh-subset materials.

Run the command printed beside each image in `gallery.md`, record wall time, and
commit both the changed images and timing table whenever an intentional renderer
change affects output.

## License

hdCodex is available under the [MIT License](LICENSE).
