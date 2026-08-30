# Shading development

This document is the living record for hdCodex shading decisions, current
coverage, and the work required for faithful MaterialX rendering. Update it
whenever a shading design decision is accepted or revised, and whenever an
implementation changes the capability table.

## Design principles

1. MaterialX is the source of truth for supported material behavior. A material
   graph must be translated through MaterialX shader generation; hdCodex must
   not recognize a high-level shader and silently replace it with a separately
   authored approximation.
2. Shader provenance is explicit. A USD-native shader is not treated as a
   MaterialX shader merely because MaterialX contains a similar NodeDef or a
   translation graph.
3. Renderer transport remains renderer-owned. Ray traversal, light and BSDF
   sampling, PDFs, multiple-importance sampling, medium tracking, and path
   termination are integration responsibilities. High-level material models
   such as OpenPBR and Standard Surface must still come from their MaterialX
   graphs rather than parallel hdCodex implementations.
4. Unsupported shaders and nodes must produce actionable diagnostics. A bound
   but unsupported material must not silently become a superficially similar
   shader.
5. Correctness is established before optimization. Optimized shader forms must
   retain the same generated MaterialX semantics and be checked against an
   inspectable reference form and gallery images.

## Accepted decisions

### SD-001: distinguish USD shaders from MaterialX shaders

Status: accepted on 2026-08-27.

`UsdPreviewSurface` authored as a USD shader is reported as a non-MaterialX,
unsupported shader. The same rule applies to other USD-native `Usd*` shader
nodes such as `UsdUVTexture` and `UsdPrimvarReader_*`; a mixed network is not
silently relabeled as MaterialX. hdCodex will not translate it into a similar
MaterialX model.

A USD material network that is genuinely authored with the MaterialX source
type and resolves to the MaterialX USD Preview Surface NodeDef is a MaterialX
material. It follows the same shader-generation and capability rules as every
other MaterialX shader. The implementation must use shader source type and
NodeDef resolution to make this distinction, not a substring match on the
shader identifier.

This means the Intel Sponza materials currently authored with USD
`UsdPreviewSurface` remain unsupported until the asset is deliberately
converted or provides a MaterialX render context.

### SD-002: generated MaterialX implementations are mandatory

Status: accepted on 2026-08-27.

Every supported MaterialX material, including OpenPBR, Standard Surface, and a
genuinely MaterialX-authored USD Preview Surface, must execute code generated
from its MaterialX graph and selected target implementations.

The former hand-lowered OpenPBR and Standard Surface parameter extractor was
removed on 2026-08-28. `SceneMaterial` is now populated only by compiling the
expanded MaterialX program from primitive NodeDefs and generic graph combiners.
The compact closure ABI and its monolithic primitive implementations remain a
transitional target until generated `eval`, `sample`, and `pdf` entry points
replace them.

Renderer-specific implementations are still required for primitive closure
operations because the MaterialX surface and volume result representation is
renderer-defined. These implementations must provide generic MaterialX closure
semantics such as BSDF evaluation, sampling, and PDF calculation. They must not
recreate complete high-level models that MaterialX already expresses as node
graphs.

### SD-003: expand MaterialX USD primvar readers through MaterialX

Status: accepted on 2026-08-27.

`ND_UsdPrimvarReader_*` is a genuine MaterialX NodeDef family and is distinct
from USD-native `UsdPrimvarReader_*` shader nodes. It must be supported when it
is part of an authored MaterialX graph.

MaterialX 1.39.3 does not propagate the `varname` interface value into the
`geomprop` input of this standard-library implementation when the reader is
nested in another NodeGraph. Before shader generation, hdCodex therefore asks
MaterialX to `flattenSubgraphs` only for nodes that resolve to an
`ND_UsdPrimvarReader_*` NodeDef. MaterialX's own graph expansion transfers the
authored value (for example `st`) to the standard `geompropvalue` node. hdCodex
does not replace the node with a hand-written texture or primvar shader.

This compatibility normalization is covered by a compiler regression test and
should be removed when the supported MaterialX version expands the nested
interface correctly without it.

## OpenPBR Playground finding

The OpenPBR Playground is genuinely authored as MaterialX. Its material files
contain `open_pbr_surface` elements feeding `surfacematerial`, and Hydra resolves
the surface to `ND_open_pbr_surface_surfaceshader`. Its upstream graphs also use
MaterialX image, geomprop, extraction, inversion, combination, color-correction,
and normal-map nodes.

hdCodex asks MaterialX 1.39.3 to generate Vulkan vertex and fragment stages and
also expands the surface NodeGraph into a dependency-ordered closure program.
The path backend compiles the supported subset of that program into its compact
closure ABI. It no longer traverses the Hydra network to find a high-level
parameter or first reachable filename. Channel operations, color correction,
anisotropy, sheen, and other operations are interpreted from that expanded
program. Uniform EDFs preserve constant partial mixes, sums, and scalar/color
products. Common normal-channel reconstruction is retained exactly. Operations
outside the compact ABI preserve an independently supported base closure and,
for texture adjustments, currently retain the source image; unsupported
terminal closures still reject.

Consequently, the scene is a valid MaterialX/OpenPBR coverage test: materials
are driven by expanded MaterialX semantics without a high-level OpenPBR field
extractor, while compact-ABI approximations remain explicit implementation debt.

One authored material is invalid independently of hdCodex: `OJfoam.mtlx`
connects the `color3` output of `mtlxcolorcorrect2` directly to the `float`
`geometry_opacity` input. MaterialX 1.39 correctly rejects that interface. The
renderer reports and omits this material; it must not invent a channel
conversion. The asset can be fixed by authoring an explicit MaterialX extract
or luminance node before `geometry_opacity`.

## MaterialX execution architecture

The current `VkShaderGenerator` output is a raster vertex/fragment program. It
can evaluate a graph for raster lighting, but a path integrator needs an
interface that can evaluate and sample closures and return their PDFs and event
properties. Vulkan cannot directly call an arbitrary function in an unrelated
fragment SPIR-V module from the current compute shader.

The intended path-tracing architecture is therefore a MaterialX code-generation
target with an hdCodex closure ABI. At minimum, generated material code needs to
provide:

- surface emission and cutout opacity;
- BSDF evaluation, sampling, PDF, and reflection/transmission classification;
- delta-event classification and IOR/medium-boundary information;
- volume absorption, scattering, emission, and phase-function operations;
- displacement where the selected rendering pipeline can apply it;
- all required uniform, texture, geomprop, tangent-frame, color-space, and unit
  resources.

OpenPBR and other high-level models should be expanded through their MaterialX
NodeGraphs into these primitive closures. The transport loop consumes the
generated ABI without knowing that a material began as OpenPBR, Standard
Surface, or another surface model.

## Performance and shader organization

Exposing closure operations logically does not require a physically fragmented
GPU program. The generated graph functions can be specialized, inlined,
constant-folded, dead-code-eliminated, and fused with the transport kernel into
a monolithic SPIR-V module. This retains a fast monolithic execution path while
keeping MaterialX as the sole material implementation.

Possible generated modes are:

1. **Fused/specialized:** the production default. Generate the closure ABI, then
   inline reachable graph and closure functions into an optimized path-tracing
   module. Constants and unused lobes should disappear. Shader variants are
   cached by graph, generator version, ABI, and compile options.
2. **Modular/instrumented:** a conformance and development mode that preserves
   clearer closure boundaries and optional tracing of lobe values, samples, and
   PDFs. It may be slower and should not define different material semantics.
3. **MaterialX raster preview:** retain the standard Vulkan vertex/fragment
   generator as an optional preview backend if a raster renderer is added. It
   is a different integration mode, not the reference for path-transport
   features.

A toggle between fused and modular generated forms is feasible. Both sides of
the toggle must originate from the same MaterialX graph and closure
implementations. A toggle back to the current hand-written OpenPBR approximation
would create two semantic sources of truth and is not part of the design.

Performance is not assumed either way. Generated graphs may increase shader
size, register pressure, compilation time, and divergence when many materially
different graphs share one kernel. Conversely, specialization can remove
inactive lobes and work that the current universal material struct always
carries. GPU timestamp benchmarks and shader statistics must determine how to
group materials and pipelines.

Candidate physical organizations to benchmark are:

- a scene-wide compute module with a generated material switch;
- graph-specialized compute pipelines with material sorting or grouped work;
- a ray-tracing-pipeline backend with generated hit/callable shader groups;
- a future raster/compute hybrid sharing the same generated material library.

No organization should be promoted based only on theoretical dispatch cost.
Gallery equivalence, white-furnace tests, shader compile/cache behavior, GPU
timestamps, occupancy, and register pressure are required evidence.

## Current capability baseline

The following describes the current hand-written compute path, not the target
generated architecture.

| Area | Current status |
| --- | --- |
| MaterialX graph generation | Raster stages plus an expanded closure program are generated; the supported program subset compiles into the path ABI |
| Arbitrary value/procedural graphs | Graph-backed helpers are recursively expanded; supported constants, direct images, channel-reconstructed normals, conditionals, luminance, clamp, combine, scalar arithmetic/trigonometry, and constant vector dot/cross/magnitude/normalize operations execute from the graph; some texture adjustments retain their source image and unsupported terminal closures reject |
| OpenPBR and Standard Surface | High-level NodeGraphs are expanded generically; supported primitive closure subsets compile without shader-name special cases |
| USD Preview Surface | USD-native networks are rejected; genuinely MaterialX-authored variants follow normal NodeGraph expansion and closure capability checks |
| Diffuse reflection | Lambert, qualitative and energy-preserving Oren-Nayar, and Burley primitives with constant or texture-driven weight/roughness; cosine-proposal indirect sampling |
| Diffuse transmission | MaterialX translucent primitive with constant or texture-driven weight/color, opposite-hemisphere direct evaluation, cosine sampling, and matching PDFs |
| Dielectric reflection | Partial IOR, Schlick Fresnel, and GGX reflection |
| Dielectric transmission | Refraction, total internal reflection, thin-wall pass-through, absorption, and homogeneous scattering; no rough microfacet BTDF |
| Conductors | Metallic base color used as F0; no complete conductor/complex-IOR semantics |
| Dispersion | Wavelength sampling with an approximate Cauchy IOR model |
| Coat | Weight, color, anisotropic roughness, and IOR; no separate authored frame or complete multi-scatter layer semantics |
| Fuzz and sheen | Conty-Kulla and Zeltner primitive modes compile to directional-albedo layers; indirect sampling currently uses a cosine proposal |
| Thin-film interference | Unsupported |
| Subsurface | Bounded eight-step spectral random walk plus an approximate wrapped direct term |
| Volume terminals | Unsupported; only one homogeneous interior transmission medium is tracked |
| Caustics | Incidental and very inefficient for environment paths; no robust caustics from analytic lights |
| Emission | Visible surface EDFs plus power-weighted emissive-triangle next-event sampling and matching BSDF-hit MIS; no volume emission |
| Lights | Hydra dome, rectangle, disk, sphere, cylinder, and distant lights plus emissive mesh triangles; analytic lights use shading-point/material-aware power selection, power-heuristic MIS covers authored domes and mesh EDFs, and automatic/lat-long domes and mesh emitters use power distributions; no portal lights or light linking |
| Geometry shaders | Displacement unsupported; subdivision uses coarse topology |
| Hair | Hair BSDF and curve geometry unsupported |
| Textures | Fixed repeat addressing, base mip only, no ray differentials, limited UDIM range, and role-based rather than graph-authored color handling |

## Missing MaterialX shading scope

Supporting only OpenPBR inputs is not equivalent to MaterialX shading support.
The generated runtime must cover the standard value, image, procedural,
coordinate, compositing, normal/bump, color-management, and unit-conversion
graphs feeding shader inputs, plus the standard shader closure families.

The installed MaterialX 1.39 PBR library includes, among others:

- Oren-Nayar and Burley diffuse, translucent, dielectric, conductor,
  generalized-Schlick, subsurface, sheen, and Chiang hair BSDFs;
- uniform, conical, measured, and generalized-Schlick EDFs;
- absorption and anisotropic VDFs;
- surface, volume, light, and displacement shader constructors;
- BSDF/EDF/VDF layer, mix, add, and multiply operations;
- anisotropic/dual roughness, artistic IOR, blackbody, and hair utilities.

OpenPBR 1.1 coverage additionally requires base weight and diffuse roughness,
base and coat anisotropy, independent normals and tangents, fuzz, coat darkening,
thin film, reference layer energy behavior, rough microfacet transmission, and
complete thin-walled behavior.

## Renderer work beyond shader generation

MaterialX graph conformance does not by itself provide complete light transport.
The renderer also needs:

- importance-sampled environment maps and emissive geometry;
- MIS coverage for emissive geometry and future analytic light representations;
- robust caustic transport;
- nested IOR and medium stacks;
- heterogeneous and emissive volumes;
- additional Hydra light and geometry types;
- displacement, subdivision refinement, and curve/hair intersection;
- ray differentials and full MaterialX texture sampling semantics;
- multiple geomprops/UV sets, authored tangent frames, and back-side shaders;
- white-furnace, lobe, PDF, gallery, and MaterialX conformance tests.

## Implementation status

Status as of 2026-08-28:

| Component | Status | Acceptance gate |
| --- | --- | --- |
| Shader provenance | Implemented | USD-native `Usd*` networks are rejected before MaterialX conversion with an actionable diagnostic |
| Mode selection | Foundation implemented | `fused`, `modular`, and `raster` are parsed as renderer settings; fused is the default |
| Fused compute pipeline | Transitional | Executes a compact ABI compiled solely from the expanded MaterialX program; generated `eval/sample/pdf` entry points remain pending |
| Modular compute pipeline | Transitional | Has an unoptimized, debug-preserving cache identity but currently shares the same compiled compact closure ABI |
| Raster preview | Program generation implemented; draw backend pending | Generated vertex/fragment SPIR-V, reflected uniforms, and graph textures must be bound by a real graphics pipeline |
| Generated material interface | Implemented | Generated SPIR-V, all public inputs, exact texture uniforms, color-space provenance, descriptor bindings, and std140 offsets survive into renderer-owned data |
| Path closure target | Foundation implemented | Expanded programs compile to the supported compact closure ABI; unsupported terminals reject while unsupported decorations preserve supported lobes; callable emission, opacity, `eval`, `sample`, and `pdf` generation remains pending |

The three setting values exist now so cache keys, scene data, tests, and the
Hydra contract do not need another incompatible transition. They must not be
described as three completed rendering backends until the acceptance gates
above pass. In particular, selecting `raster` currently reports that its
graphics backend is not complete rather than falling back to compute.

## Prioritized implementation plan

### Phase 0: provenance and generated-program plumbing

Status: complete for the current Vulkan/MaterialX 1.39.3 foundation.

- Reject USD-native shader networks before MaterialX document creation.
- Preserve the authored terminal NodeDef identifier.
- Retain generated raster SPIR-V plus every public uniform and filename input.
- Load every generated graph texture with its MaterialX color-space metadata.
- Reflect descriptor sets, bindings, and uniform member offsets from SPIR-V.
- Add one concise diagnostic per unsupported material and avoid cascading
  MaterialX conversion warnings.

Exit gate: Sponza reports its USD Preview Surface networks as unsupported;
OpenPBR Playground reaches generation with complete graph resources.

### Phase 1: hdCodex MaterialX closure target

Status: in progress. The expanded graph now compiles to a compact
closure ABI without high-level shader-name cases. Generated callable
`eval`/`sample`/`pdf` entry points and broader primitive coverage remain.

- Derive a MaterialX GLSL/Vulkan generator target with a renderer closure ABI.
- Expand high-level NodeGraph implementations, including OpenPBR and Standard
  Surface, through MaterialX exactly once.
- Represent primitive BSDF/EDF/VDF and mix, add, multiply, and layer operations
  as a bounded generated closure program.
- Implement generic primitive closure `eval`, `sample`, and `pdf` operations,
  including event flags, eta, delta classification, emission, and opacity.
- Generate texture, geomprop, normal/tangent, color, and unit operations into
  the same program. Do not traverse upstream merely to find a filename.
- Add ABI validation that fails generation if a reachable closure primitive has
  no renderer implementation.

Exit gate: OpenPBR Playground no longer reads any high-level OpenPBR fields from
`SceneMaterial`; its lobes and upstream value graph come from generated code.

### Phase 2: complete the three physical backends

- **Fused/specialized (default):** link reachable generated graph and primitive
  closure operations into the transport compute module; enable optimization,
  constant folding, and dead-lobe removal; cache by scene material set and ABI.
- **Modular/instrumented:** retain graph and closure boundaries, expose lobe
  IDs/weights/eval/pdf/sample diagnostics, and dispatch material-grouped work
  without changing semantics.
- **Raster preview:** use the shared graphics-and-compute Vulkan queue, flatten
  face-corner geometry, create color/depth targets, bind reflected generated
  uniform and texture descriptors, and draw with the unmodified generated
  MaterialX vertex/fragment modules.

Exit gate: all three modes render the same opaque diffuse conformance scenes;
fused and modular agree numerically for `eval/pdf`, and raster is visibly
labeled as a lighting preview rather than a path-transport reference.

### Phase 3: complete PBR closure coverage

- Implemented: MaterialX qualitative and energy-preserving Oren-Nayar plus
  Burley rough-diffuse evaluation, texture-driven weight/roughness, and matching
  indirect sample weights.
- OpenPBR 1.1: complete fuzz reference sampling, independent normal/tangent
  frames, thin film, rough microfacet transmission, thin-wall
  behavior, reference layers, and energy compensation.
- Standard Surface and MaterialX USD Preview Surface through their authored
  NodeGraphs, with no shader-name special cases in transport.
- Conductor complex IOR, dielectric boundaries, subsurface, hair, and the
  remaining standard closure combiners. Diffuse translucent transport is
  implemented independently from subsurface.
- White-furnace, reciprocity, normalization, sample/eval/pdf consistency, and
  MaterialX reference-image tests for every primitive.

### Phase 4: direct lighting and MIS

- Implemented: power-heuristic MIS between authored-dome next-event samples
  and non-delta diffuse, sheen, coat, and anisotropic-GGX continuation paths.
- Implemented: luminance/solid-angle texture-importance sampling for automatic
  and lat-long dome lights, with matching direct and continuation-path PDFs.
- Implemented: Hydra rectangle, disk, sphere, cylinder-side, and
  angular-diameter distant-light sampling with world-space transforms,
  normalization, shaping, and shadow controls.
- Implemented: area/luminance-power sampling of emissive triangles with exact
  texture/opacity evaluation, visibility, solid-angle PDFs, and BSDF-hit MIS.
- Implemented: shading-point- and material-aware power sampling across
  analytic lights, including exact selection-mixture PDFs for dome MIS.
- Extend BSDF/light MIS to every non-delta analytic light representation.
- Complete portal/geometry lights, IES shaping, visibility categories, and
  light linking.
- Consistent spectral/RGB policy between generated closures and light sampling.

Exit gate: small bright emitters and HDR environments converge without the
current high variance or systematic bias.

### Phase 5: transmission, media, subsurface, and caustics

- Nested dielectric/medium stack with correct eta tracking.
- Homogeneous then heterogeneous absorption, scattering, emission, and phase
  functions generated from MaterialX VDF/volume terminals.
- Replace the fixed subsurface approximation with a validated random-walk or
  diffusion implementation selected by primitive closure semantics.
- Add robust caustic transport after baseline MIS: benchmark path guiding,
  manifold next-event estimation, and photon/vertex-connection approaches
  before choosing the production technique.

### Phase 6: geometry shading features

- Displacement and height-to-normal semantics.
- Refined subdivision surfaces and correct limit normals/tangents.
- Curves plus Chiang hair closures, motion/deformation blur, and back-side
  material terminals.

### Phase 7: texture and geometric-property conformance

- Ray differentials or cone footprints for mip/anisotropic filtering.
- All MaterialX address/filter/frame/UDIM semantics and multiple UV sets.
- Arbitrary geomprops, authored tangent frames, color management, and units.
- Texture residency and descriptor strategies that do not impose a fixed
  256-texture scene limit.

### Phase 8: conformance claim and performance qualification

- Run the MaterialX render-test corpus and maintain an explicit support matrix
  for standard PBR, NPR, volume, displacement, light, and custom NodeDefs.
- Rerender every gallery entry for render-affecting changes and version image
  diffs with the implementation commit.
- Benchmark compile time, cache reuse, shader size, registers, occupancy,
  divergence, GPU time, and memory for fused versus modular grouping choices.
- Claim full MaterialX support only for the tested target/library/version and
  publish unsupported-node diagnostics for everything outside it.

## Open questions

- Which closure ABI representation gives MaterialX graphs enough expressive
  power without imposing excessive per-path storage?
- Should the first generated backend assemble one scene-wide compute module or
  graph-specialized pipelines?
- Which MaterialX conformance corpus and numeric/image tolerances define the
  support claim?
- Which caustic algorithm provides the best quality/performance balance after
  baseline BSDF/light MIS is correct?

## Primary references

- [MaterialX 1.39 specification](https://materialx.org/Specification.html)
- [MaterialX physically based shading specification](https://github.com/AcademySoftwareFoundation/MaterialX/blob/main/documents/Specification/MaterialX.PBRSpec.md)
- [MaterialX shader-generation guide](https://github.com/AcademySoftwareFoundation/MaterialX/blob/main/documents/DeveloperGuide/ShaderGeneration.md)
- [OpenPBR specification](https://academysoftwarefoundation.github.io/OpenPBR/)
- [OpenPBR parameter reference](https://github.com/AcademySoftwareFoundation/OpenPBR/blob/main/parametrization.md.html)
