# Neural rendering and DLSS roadmap

Status: approved architecture plan; implementation not started

Last reviewed: 2026-08-30

Reference renderer: spectral Vulkan ray-query path tracer

Primary owner: renderer/GPU subsystem

## How to use this plan

This is the durable handoff and progress record for temporal reconstruction,
NVIDIA DLSS, and the GPU-frame work they require. Update the phase tracker,
acceptance evidence, and decision log in the same commit as each completed
milestone. Do not mark a phase complete merely because its API exists: its exit
gate and required tests must pass.

When resuming after a pause:

1. Read the decision log and phase tracker below.
2. Inspect the live worktree and recent commits; never assume this document is
   newer than the code.
3. Run `compile.bat`, the full test suite, and `validate_usd.bat` before changing
   the renderer baseline.
4. Work on only one reviewable phase boundary at a time.
5. Rerender and commit the complete 1024-pixel/1024-SPP gallery after every
   render-affecting change.
6. Record timing, image, validation, and hardware evidence in the completed
   phase entry.

## Phase tracker

| Phase | Deliverable | State | Evidence/commit |
| --- | --- | --- | --- |
| 0 | Baseline protection, validation, and timing instrumentation | Not started | - |
| 1 | Headless NVIDIA integration feasibility spike | Not started | - |
| 2 | GPU frame resources and reference-preserving readback | Not started | - |
| 3 | Temporal camera, jitter, counters, and reset contract | Not started | - |
| 4 | Depth and camera-motion guide foundation | Not started | - |
| 5 | MaterialX-derived reconstruction guides | Blocked | Requires generated closure ABI |
| 6 | Native GPU reconstruction fallback | Not started | - |
| 7 | NVIDIA backend bootstrap and runtime support query | Blocked | Requires Phase 1 decision |
| 8 | DLSS Super Resolution and DLAA | Blocked | Requires Phase 7 |
| 9 | DLSS Ray Reconstruction | Blocked | Requires Phases 5, 7, and 8 |
| 10 | Stable object/deformation motion | Blocked | Requires reusable BLAS/TLAS work |
| 11 | Presentation features in a separate viewer | Deferred | Requires a viewer/host project |
| 12 | Future DLSS feature adapter | Blocked | Waiting for a public production SDK |

Allowed states are `Not started`, `In progress`, `Blocked`, `Complete`, and
`Deferred`. Add a short reason whenever a phase is blocked.

## Objective

Add optional real-time temporal reconstruction without weakening hdCodex's role
as a portable, progressive, physically based Hydra path tracer.

The target architecture must support:

- the existing full-resolution progressive reference renderer;
- a renderer-owned spatial/temporal fallback on every supported Vulkan GPU;
- NVIDIA Super Resolution and DLAA on supported systems;
- NVIDIA Ray Reconstruction after its semantic guide buffers are correct;
- future reconstruction backends through the same renderer-neutral boundary;
- a later swapchain-owning viewer for Frame Generation and latency features.

The reference path is authoritative. Neural output is an interactive
reconstruction and must never silently replace offline convergence or gallery
baselines.

## Non-goals

- Do not add NVIDIA-only requirements to the default build.
- Do not change MaterialX shading semantics to accommodate a denoiser.
- Do not derive guide buffers from OpenPBR or Standard Surface names.
- Do not feed display-encoded, tone-mapped, or raw CIE XYZ values into a
  reconstruction backend expecting linear RGB.
- Do not put Frame Generation, Multi Frame Generation, or Reflex inside a Hydra
  delegate that does not own presentation.
- Do not expose a setting for an unreleased feature or infer private APIs.
- Do not use DLSS to hide avoidable synchronization stalls, broken MIS,
  fireflies, NaNs, or biased transport.

## Current baseline

At the time of this review:

- `VulkanContext` creates a private Vulkan 1.3 instance, physical device,
  logical device, and one graphics/compute queue.
- The context enables a fixed set of ray-query/acceleration-structure features;
  there is no pre-device extension provider for optional backends.
- `VulkanPathTracer` writes progressive linear RGBA32F into a device-local
  storage buffer.
- A single command buffer and fence serialize submissions. Each render waits,
  copies the entire output to a host-visible buffer, waits again, and returns a
  `std::vector<float>`.
- `HdCodexRenderPass` copies that vector into a CPU-backed Hydra render buffer.
- Camera movement traces at half resolution with two bounces and performs
  nearest-neighbour scaling on the CPU.
- Color is the only populated AOV. Depth is advertised but only cleared.
- Camera, scene, settings, and size changes reset progressive accumulation.
- There is no frame index separate from the progressive sample index, no
  subpixel jitter, no previous camera state, no motion field, and no temporal
  history.
- Gallery output is captured to scene-linear EXR with usdrecord color
  correction disabled, then converted to display JPEG after rendering.

These constraints make the GPU-frame refactor useful independently of DLSS.

## Architectural decisions

### Preserve two different accumulation contracts

Reference mode keeps the current persistent unbiased average:

```text
path sample 1 + ... + path sample N
                 -> RGBA32F persistent average
                 -> optional readback/display transform
```

Real-time Ray Reconstruction mode creates a new noisy frame every displayed
frame:

```text
1..samplesPerFrame fresh paths
                 -> current-frame noisy linear RGB
                 -> reconstruction backend owns long temporal history
```

Never combine an unbounded renderer average with a temporal neural history.
Maintain three independent counters:

- `referenceSampleIndex`: persistent only in reference mode;
- `frameIndex`: increments once per submitted temporal frame;
- `sampleWithinFrame`: zero through `samplesPerFrame - 1`.

Returning to reference mode must restore deterministic reference sampling after
an explicit reset.

### Make GPU frame resources the internal boundary

The long-term render result is a renderer-owned set of Vulkan images plus frame
metadata. CPU readback is an adapter, not the path tracer's primary return type.

Conceptual types:

```cpp
struct GpuFrameResources {
    VulkanImage noisyColor;
    VulkanImage depth;
    VulkanImage motion;
    VulkanImage normalRoughness;
    VulkanImage diffuseAlbedo;
    VulkanImage specularAlbedo;
    VulkanImage reconstructedColor;
    VulkanImage exposure;
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t outputWidth;
    uint32_t outputHeight;
};

struct FrameMetadata {
    Matrix4 currentWorldToView;
    Matrix4 currentViewToClip;
    Matrix4 previousWorldToView;
    Matrix4 previousViewToClip;
    Vec2 jitterPixels;
    Vec2 previousJitterPixels;
    Vec2 motionVectorScale;
    float preExposure;
    uint64_t frameIndex;
    bool cameraCut;
    bool historyReset;
};

class TemporalReconstructionBackend {
public:
    virtual SupportInfo QuerySupport(const BackendContext&) = 0;
    virtual void Resize(const Resolution&) = 0;
    virtual void Evaluate(VkCommandBuffer, const GpuFrameResources&,
                          const FrameMetadata&) = 0;
    virtual void ResetHistory() = 0;
};
```

Keep NVIDIA types out of public Hydra-facing and renderer-neutral headers.

### Keep reference precision separate from SDK formats

Do not replace the reference RGBA32F accumulation buffer with RGBA16F. Resolve
or convert only the interactive current-frame result to the SDK boundary.

Initial interactive formats, subject to device format queries and the selected
SDK guide:

| Resource | Candidate format | Contract |
| --- | --- | --- |
| Noisy linear color | `VK_FORMAT_R16G16B16A16_SFLOAT` | Linear RGB, HDR, current frame only |
| Reconstructed color | `VK_FORMAT_R16G16B16A16_SFLOAT` | Output resolution, before display transform |
| Linear depth | `VK_FORMAT_R32_SFLOAT` | Same accepted primary hit as motion |
| Motion | `VK_FORMAT_R16G16_SFLOAT` | Dense, documented direction and units |
| Normal + roughness | `VK_FORMAT_R16G16B16A16_SFLOAT` | Documented normal space; linear roughness |
| Diffuse albedo | `VK_FORMAT_R16G16B16A16_SFLOAT` | Linear MaterialX-derived reflectance |
| Specular albedo | `VK_FORMAT_R16G16B16A16_SFLOAT` | Linear MaterialX-derived reflectance |
| Exposure | `VK_FORMAT_R32_SFLOAT` | 1x1, or documented backend auto-exposure |

Before allocation, query format features for the exact usage combination.
Images need only the storage, sampled, transfer, attachment, or SDK usages they
actually require; do not assume every format supports a maximal union.

### Keep the color pipeline explicit

Reconstruction input is:

- the spectral sensor result converted to documented linear Rec.709/sRGB
  primaries;
- HDR and finite;
- pre-exposed only if the exact matching exposure is supplied;
- before neutral tone mapping, sRGB transfer, UI, sharpening, or another
  temporal filter.

The gallery display transform is output-only and must never feed back into
reconstruction.

### Derive guides from MaterialX semantics

Depth and motion are geometric, but diffuse/specular albedo and roughness must
come from the expanded generated MaterialX closure program. Arbitrary `mix`,
`add`, `multiply`, `layer`, conductor, transmission, coat, sheen, subsurface,
and future custom closures cannot be reconstructed reliably from high-level
shader fields.

Phase 5 is therefore blocked until the generated closure ABI can expose stable
full-RGB guide evaluation independently of the stochastic wavelength packet.
No OpenPBR/Standard Surface name-specific extraction is permitted.

### Treat vendor integration as optional and process-sensitive

The default build and plugin must run without NVIDIA libraries. Vendor code
belongs in a separate library/translation unit and must fail over to the native
backend without preventing the Hydra plugin from loading.

However, Vulkan feature requirements must be known before instance/device
creation. Optional does not mean late: the device bootstrap must accept a
pre-device requirement provider selected before `VulkanContext` is constructed.

Streamline also has process-wide hooking, initialization, garbage-collection,
and presentation assumptions. A Hydra plugin loaded into a larger host must not
interpose unrelated Vulkan devices. Phase 1 decides whether headless Streamline,
direct Vulkan NGX, or a viewer-owned integration is viable before production
vendor work starts.

### Keep presentation outside the delegate

Frame Generation, Multi Frame Generation, Dynamic Multi Frame Generation, and
Reflex depend on swapchain ownership, presentation timing, UI separation, and
latency markers. They belong in a separate viewer/host milestone.

The private hdCodex device also means a future viewer must either use the same
`VulkanContext` or implement explicit external-memory and external-semaphore
interop. A raw `VkImage` cannot be consumed by an unrelated device.

## Phase 0: protect the baseline and measure it

State: Not started.

Work:

- [ ] Record gallery reference hashes and wall times.
- [ ] Add GPU timestamp queries around tracing, copies, and future postpasses.
- [ ] Record CPU fence-wait and render-pass time separately.
- [ ] Record readback bytes and latency.
- [ ] Add optional Vulkan validation-layer/debug-utils setup without changing
      production defaults.
- [ ] Query and record queue timestamp support and valid-bit precision.
- [ ] Preserve current correctness tests and USD validation.

Exit gate:

- Repeatable reference evidence exists for Chess, Sponza, OpenPBR, Glass,
  Kitchen, and CollectiveProject.
- Timing separates tracing, synchronization, readback, and CPU AOV copying.
- Validation-layer smoke runs produce no relevant errors.

Rollback point: instrumentation can be disabled without affecting render data.

## Phase 1: headless NVIDIA feasibility spike

State: Not started.

This phase must precede any production Streamline code.

Work:

- [ ] Obtain the licensed production SDK/package and record exact Streamline,
      NGX, plugin, driver, and model versions.
- [ ] Verify redistribution, application-ID, notification, signature, and
      production-binary requirements.
- [ ] Test Streamline manual Vulkan integration with the delegate's private
      instance/device and no owned swapchain.
- [ ] Determine how required per-frame Streamline bookkeeping/garbage
      collection can legally and safely run without host presentation access.
- [ ] Test direct Vulkan NGX integration if the production SDK supports it.
- [ ] Confirm that initialization does not interpose or mutate Vulkan/D3D
      devices owned by the host process or other plugins.
- [ ] Query required instance extensions, device extensions, feature chains,
      and queues before creating the hdCodex device.
- [ ] Confirm teardown ordering when the host unloads/reloads the delegate.
- [ ] Produce a minimal support-query result on NVIDIA, AMD, and Intel systems.

Decision gate:

Choose exactly one:

1. Streamline can run safely inside the headless delegate.
2. Direct NGX can run safely inside the headless delegate.
3. NVIDIA reconstruction must move to a swapchain-owning viewer/host.

Record the evidence and chosen integration in the decision log. If none is
safe, Phases 7-9 remain blocked while Phases 2-6 may continue.

## Phase 2: GPU frame resources and readback

State: Not started.

Likely files:

- `include/hdcodex/gpu/vulkan_context.h`
- `src/gpu/vulkan_context.cpp`
- `include/hdcodex/gpu/vulkan_path_tracer.h`
- `src/gpu/vulkan_path_tracer.cpp`
- new GPU image/frame-resource sources
- `src/gpu/CMakeLists.txt`

Work:

- [ ] Add RAII wrappers for images, views, memory, formats, and tracked layouts.
- [ ] Add a pre-device requirement provider to Vulkan bootstrap.
- [ ] Allocate render-resolution and output-resolution resources separately.
- [ ] Keep RGBA32F reference accumulation unchanged.
- [ ] Add a current-frame interactive resolve into a GPU image.
- [ ] Replace the single submit-and-wait helper with explicit frame submission
      and fence/timeline ownership.
- [ ] Enable/query timeline-semaphore features if timeline retirement is used.
- [ ] Add a two- or three-slot readback ring for interactive Hydra consumers.
- [ ] Retain deterministic synchronous readback for tests and `usdrecord`.
- [ ] Retire resized resources only after their GPU work completes.

Exit gate:

- Reference images remain equivalent within the documented tolerance.
- Interactive color completes into a Vulkan image before optional readback.
- Vulkan validation reports correct lifetimes, layouts, and synchronization.
- CPU and GPU timing proves that interactive readback can overlap useful work.

Rollback point: the synchronous adapter still produces the existing CPU AOV.

## Phase 3: temporal camera and history contract

State: Not started.

Work:

- [ ] Store full current and previous world/view/clip transforms.
- [ ] Define row/column-major conversion once and test it.
- [ ] Generate a deterministic, documented subpixel jitter sequence.
- [ ] Apply jitter to primary rays while supplying unjittered camera matrices
      and pixel-space jitter to reconstruction backends.
- [ ] Define motion direction and coordinate convention explicitly.
- [ ] Separate `frameIndex`, `sampleWithinFrame`, and
      `referenceSampleIndex`.
- [ ] Centralize history reset for first frame, cuts, scene revisions, material
      changes, resize, render-scale/mode changes, exposure changes, unsupported
      animation, and backend errors.
- [ ] Ensure one reset pulse is emitted for each invalidating event.
- [ ] Retire old-mode GPU work before reinterpreting frame resources.

Exit gate:

- Unit tests cover matrix conversion, jitter, counters, and reset transitions.
- Smooth camera motion preserves history; cuts and edits reset exactly once.
- Returning to reference mode remains reproducible.

## Phase 4: depth and camera-motion guides

State: Not started.

Work:

- [ ] Write deterministic first-hit depth from the same accepted opacity/cutout
      surface used by noisy color.
- [ ] Select and document view-space linear depth or hardware depth and use its
      matching backend tag/constants.
- [ ] Define stable miss/background depth and invalid-motion values.
- [ ] Generate camera-only motion for static geometry by projecting the current
      accepted world-space hit through current and previous unjittered matrices.
- [ ] Specify whether motion includes camera motion and whether values are
      pixels or normalized coordinates; supply the exact matching scale.
- [ ] Reset history for every scene revision, deformation, topology change, or
      unknown transform history.
- [ ] Expose depth and motion as optional debug Hydra AOVs.
- [ ] Add CPU reference projection tests and debug image exports.

Do not emit plausible zero motion for moving geometry. Resetting is correct
until previous object/deformation state exists.

Exit gate:

- Camera motion agrees with the CPU reference within tolerance.
- Depth and motion describe the same accepted surface at cutout boundaries.
- Disocclusions and misses use documented values.
- Debug AOVs are finite and correctly oriented.

## Phase 5: MaterialX-derived reconstruction guides

State: Blocked by generated closure ABI.

Work:

- [ ] Define renderer-neutral guide outputs in the MaterialX closure ABI.
- [ ] Evaluate stable full-RGB diffuse albedo, specular albedo, linear
      roughness, shading normal, and any required material masks.
- [ ] Preserve semantics through arbitrary closure mix/add/multiply/layer
      operations.
- [ ] Define guide behavior for conductor, dielectric transmission, coat,
      sheen/fuzz, subsurface, emission, volumes, thin walls, and unsupported
      closures.
- [ ] Keep guides deterministic and independent of path RNG/wavelength choice.
- [ ] Add optional specular hit distance or specular motion only after image
      tests show which official input is required.
- [ ] Provide debug AOVs and MaterialX conformance scenes for guide semantics.

Exit gate:

- No guide is derived from high-level OpenPBR/Standard Surface names.
- Closure combiners produce documented, bounded guides.
- Guides are finite, linear, and stable across frames.
- Reference transport output is unchanged.

## Phase 6: native GPU reconstruction fallback

State: Not started.

Work:

- [ ] Replace CPU nearest-neighbour interactive scaling with a GPU spatial
      scaler first.
- [ ] Add a small renderer-owned temporal fallback only after Phase 3/4 inputs
      are validated.
- [ ] Keep native and reference modes available on every supported vendor.
- [ ] Route all postprocessing before optional readback.
- [ ] Add settings without vendor-version names:

```text
reconstructionBackend = reference | native | nvidia
reconstructionFeature = spatial | temporal | superResolution | rayReconstruction
qualityMode            = native | quality | balanced | performance
interactiveScale       = implementation-defined supported range
samplesPerFrame        = 1 .. documented maximum
```

- [ ] Validate history/reset behavior independently of a proprietary SDK.

Exit gate:

- CPU nearest scaling is no longer on the interactive path.
- Unsupported/missing vendor backends fall back without losing output.
- `usdrecord` and Hydra CPU AOV consumers still work.

## Phase 7: NVIDIA backend bootstrap

State: Blocked by Phase 1 decision.

Work:

- [ ] Add `HDCODEX_ENABLE_NVIDIA_RECONSTRUCTION`, default `OFF` until stable.
- [ ] Use an explicit SDK root; do not fetch proprietary binaries implicitly.
- [ ] Compile vendor code in a private library and load production signed
      binaries securely at runtime.
- [ ] Feed Phase 1 requirements into instance/device creation.
- [ ] Match the backend adapter to the exact selected `VkPhysicalDevice` using
      stable identifiers, not only vendor/name heuristics.
- [ ] Provide instance, physical device, device, queues, command buffers, and
      synchronization through the chosen supported integration.
- [ ] Implement support query and clear fallback diagnostics.
- [ ] Restore Vulkan state/layout assumptions after backend evaluation.
- [ ] Exercise repeated create/destroy/resize/plugin reload cycles.

Exit gate:

- Missing SDK, unsupported GPU, unsupported OS/driver, and backend errors all
  preserve native/reference output.
- AMD and Intel devices remain usable.
- The host process is not globally interposed unexpectedly.

## Phase 8: Super Resolution and DLAA

State: Blocked by Phase 7.

Work:

- [ ] Tag linear HDR input/output, depth, motion, and exposure with exact
      extents, layouts, formats, and lifecycles.
- [ ] Supply current frame constants, unjittered matrices, jitter, reset, motion
      scale, depth convention, and exposure.
- [ ] Query backend-recommended fixed render extents per quality mode.
- [ ] Implement native-resolution DLAA.
- [ ] Implement Quality/Balanced/Performance Super Resolution.
- [ ] Treat SR as an upscaler, not a denoiser for raw 1-4 SPP path tracing.
- [ ] Document the minimum input cleanliness/accumulation expected by SR mode.
- [ ] Verify camera motion, cuts, resize, and mode transitions with the vendor
      debug overlay/logs.

Exit gate:

- DLAA and SR pass official input validation on supported hardware.
- Missing/failed vendor evaluation falls back for that viewport.
- No display transform is applied before reconstruction.

## Phase 9: Ray Reconstruction

State: Blocked by Phases 5, 7, and 8.

Work:

- [ ] Select RR as a distinct feature, not a synonym for SR.
- [ ] Clear noisy radiance every temporal frame and average only
      `samplesPerFrame` fresh samples.
- [ ] Continue advancing stochastic samples while the camera is stationary.
- [ ] Supply linear HDR noisy color plus validated depth, dense motion,
      normals, linear roughness, diffuse albedo, and specular albedo.
- [ ] Add specular motion or hit-distance guides if required by the production
      SDK and quality tests.
- [ ] Respect current SDK restrictions such as fixed input extent where
      applicable; a scale change resets/recreates history.
- [ ] Test glass, rough reflection, emissives, cutouts, normal maps,
      subsurface, volumes, thin geometry, and deformation.
- [ ] Compare reconstructed frames against high-sample references while
      labeling the result temporal, not mathematically converged.

Exit gate:

- One/few-sample output is temporally stable in representative scenes.
- A stationary view receives new noisy samples without a second persistent
  renderer history.
- No guide contains NaNs, stale data, mixed spaces, or display-transformed
  color.
- Reference mode remains equivalent to its baseline.

## Phase 10: object, instance, and deformation motion

State: Blocked by reusable BLAS/TLAS work.

Dependencies overlap `hybrid-renderer-plan.md` and `subdivision-plan.md`:

- per-prototype geometry buffers and BLAS objects;
- native TLAS instances with stable instance/primitive identity;
- previous/current instance transforms;
- previous/current skinned or deforming positions;
- BLAS update/refit with rebuild fallback;
- history invalidation for topology/material/subset changes.

Work:

- [ ] Add rigid object and PointInstancer motion.
- [ ] Add UsdSkel/deformation motion after previous positions are reliable.
- [ ] Define disocclusion and topology-change policy.
- [ ] Add animated-scene motion/reference tests.

Exit gate:

- Moving objects no longer require global history reset when their motion is
  representable.
- Instanced and skinned scenes do not retain stale history.

## Phase 11: presentation features in a separate viewer

State: Deferred.

Build or extend a host that owns:

- the Vulkan device or explicit cross-device resource sharing;
- window, swapchain, presentation loop, and pacing;
- HUD-less and separate UI color/alpha resources;
- Reflex markers and latency measurement;
- Frame Generation/Multi Frame Generation lifecycle;
- resize, fullscreen, pause, loading, and swapchain recreation.

Do not force these concerns into `HdCodexRenderPass`.

## Phase 12: future DLSS feature adapter

State: Waiting for a public production SDK.

Do not expose a `dlss5` setting or guess inputs, formats, hardware support, or
ordering. When an official production SDK ships:

- [ ] Read its integration guide and license.
- [ ] Extend the renderer-neutral frame contract only for documented inputs.
- [ ] Add a runtime-reported feature adapter rather than versioning public mode
      names.
- [ ] Preserve immediate native/SR/RR fallback.
- [ ] Validate temporal and semantic fidelity for skin, hair, fabric,
      translucency, stylized assets, and identity-critical content.

## Validation matrix

Every temporal backend must cover:

| Scenario | Required behavior |
| --- | --- |
| Static camera and scene | Stable result; no crawling; temporal frames continue when required |
| Slow camera motion | Correct motion direction and minimal ghosting |
| Camera cut | One-frame reset |
| Resize or scale/mode change | Safe retirement/recreation and reset |
| Scene/material/light edit | Reset until a finer policy is proven |
| Point instancing | No stale instance history |
| Skinned/deforming mesh | Reset initially; correct motion after Phase 10 |
| Alpha cutouts | Depth/motion use the accepted visible surface |
| Glass/transmission | Documented guide behavior; no foreground smearing |
| Emissive/HDR lighting | Finite unclipped reconstruction input |
| Subsurface/volume | Documented guide composition |
| AMD/Intel GPU | Reference/native paths remain functional |
| Missing/outdated NVIDIA runtime | Actionable diagnostic and fallback |
| Headless `usdrecord` | Deterministic synchronous reference capture |
| Reference <-> temporal transition | Both histories reset safely |
| Fireflies/extreme samples | No NaNs; no silent biased clamp |

Automated coverage must include projection math, jitter, motion scaling,
history state transitions, counter independence, image format/layout mapping,
fallback selection without proprietary binaries, reference image comparisons,
validation-layer smoke tests, and repeated lifetime/resize cycles.

## Performance gates

Measure at minimum:

- trace GPU time;
- guide-generation GPU time;
- reconstruction GPU time;
- copy/readback GPU time and bytes;
- CPU render-pass and fence-wait time;
- GPU memory for current/history resources;
- actual rendered-frame rate;
- presented-frame rate and input latency only in a presentation-owning viewer.

Do not claim a win from displayed FPS alone. The first independent win is
removing serialized readback and CPU nearest scaling from the interactive
critical path while preserving deterministic capture.

## Change boundaries

Prefer one commit series per boundary:

1. Measurement and validation infrastructure.
2. NVIDIA lifecycle feasibility report - no production dependency.
3. Vulkan image/resource RAII.
4. GPU color target with compatibility readback.
5. Temporal metadata and state-machine tests.
6. Depth and camera motion.
7. MaterialX guide ABI and guide tests.
8. Native GPU reconstruction.
9. Optional vendor bootstrap/support query.
10. SR/DLAA.
11. RR.
12. Object/deformation motion.
13. Presentation viewer.

Never combine device bootstrap, guide semantics, temporal math, and proprietary
SDK evaluation into one patch.

## Milestone definitions

### Foundation milestone: no proprietary SDK required

Complete when:

- GPU-resident interactive color, depth, motion, normal/roughness, and albedo
  images exist and can be visualized;
- temporal metadata/reset behavior has automated tests;
- camera motion is correct for static scenes;
- the Hydra CPU AOV path works through explicit readback;
- reference RGBA32F progressive rendering is unchanged;
- native GPU scaling replaces CPU nearest-neighbour scaling;
- a test/native backend exercises fresh-per-frame versus reference accumulation;
- reconstruction input is documented linear RGB before display transformation.

### NVIDIA SR/DLAA milestone

Complete when the chosen supported integration passes runtime support checks,
official input validation, camera/resize/reset tests, and non-NVIDIA fallback.

### NVIDIA RR milestone

Complete when MaterialX-derived guide semantics are validated, fresh-frame
sampling is implemented, representative scenes are temporally stable, and the
reference path remains unchanged.

## SDK/version policy

- Pin the complete production package and record Streamline, NGX, feature
  plugin, model, driver, and Vulkan versions together.
- Do not mix a newer public Streamline framework with older binary feature
  plugins unless NVIDIA documents that combination as supported.
- Do not use development/watermarked binaries in release packaging.
- Verify binary signatures and load only from controlled paths.
- Use an explicit local SDK root; never silently download proprietary SDKs from
  CMake.
- Query support at runtime instead of inferring it from an `RTX` name.

## Decision log

Append decisions; do not rewrite history. Superseding entries should reference
the earlier decision.

| Date | Decision | Rationale | Consequence |
| --- | --- | --- | --- |
| 2026-08-30 | Keep full-resolution progressive rendering as the reference path | Neural reconstruction is temporal and not an unbiased convergence result | Gallery and offline capture remain reference-only |
| 2026-08-30 | Capture reconstruction before the display transform | Backends consume linear HDR working RGB | Gallery tone mapping/sRGB is output-only |
| 2026-08-30 | Block RR guides on the generated MaterialX closure ABI | High-level shader-field extraction violates pure MaterialX and cannot represent arbitrary closure graphs | Phase 5 waits for callable closure/guide support |
| 2026-08-30 | Require a headless lifecycle spike before choosing Streamline | Manual Streamline integration has device-bootstrap and presentation bookkeeping requirements | Vendor production phases remain blocked pending evidence |
| 2026-08-30 | Keep Frame Generation and Reflex outside the Hydra delegate | The delegate does not own the swapchain or presentation timing | These features require a separate viewer/host |
| 2026-08-30 | Keep RGBA32F reference accumulation separate from RGBA16F SDK inputs | Interactive boundary formats must not reduce reference precision | Phase 2 maintains two explicit resource contracts |

## Primary references

- [NVIDIA Streamline programming guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md)
- [NVIDIA Streamline manual Vulkan hooking guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideManualHooking.md)
- [NVIDIA DLSS Super Resolution guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md)
- [NVIDIA DLSS Ray Reconstruction guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md)
- [NVIDIA DLSS developer page](https://developer.nvidia.com/rtx/dlss)
- [Hybrid renderer plan](hybrid-renderer-plan.md)
- [MaterialX shading development plan](shading_development.md)
- [Subdivision mesh plan](subdivision-plan.md)
