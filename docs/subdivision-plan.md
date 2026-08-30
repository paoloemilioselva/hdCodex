# Subdivision mesh plan

## Status

The CPU uniform-refinement milestone is implemented. Catmull-Clark,
Loop, and bilinear topology, subdivision tags, holes/topological invisibility,
orientation, the primary texture-coordinate primvar, material-subset mapping,
and limit positions/normals reach the path tracer. Renderer settings expose an
on/off toggle and a level 0–8 override. Focused tests cover all three schemes,
face-varying seams, uniform primvars, holes, creases, corners, and coarse-face
mapping.

Topology refiners, emitted topology, coarse-face maps, and vertex, varying, and
face-varying stencil tables are cached per mesh. Animated point and primvar
values reevaluate against those tables; topology, tag, level, or face-varying
index changes rebuild the cache. Tests assert both reuse and invalidation.

Still required before Phase 2 is complete: cover arbitrary retained primvars
rather than only the renderer's primary UV set, add Hydra dirty-bit integration
tests, and add checked-in focused image comparisons.

## Scope and constraints

Subdivision support will be built out of tree using public OpenUSD and
OpenSubdiv APIs. It will not copy `hdSt`, `hdEmbree`, or any private OpenUSD
implementation. The standalone build will link the `pxOsd`/OpenSubdiv libraries
from `HDCODEX_OPENUSD_ROOT`; any future Houdini-compatible build must remain a
separate preset and dependency graph.

The first target is deterministic uniform refinement for Catmull-Clark, Loop,
and bilinear meshes. Adaptive limit-surface tessellation is deliberately a later
performance/quality phase rather than a prerequisite for correct Hydra support.

## Phase 1: correct uniform refinement

1. Extend `HdCodexMesh::Sync()` to retain the subdivision scheme, display-style
   refine level, orientation, holes, and `PxOsdSubdivTags`. Include these plus
   coarse topology and face-varying topology in a stable refinement-cache key.
2. Convert `HdMeshTopology::GetPxOsdMeshTopology()` through
   `PxOsdRefinerFactory::Create()`, then call
   `OpenSubdiv::Far::TopologyRefiner::RefineUniform()` at the requested level.
   Polygonal meshes whose scheme is `none` continue through the existing path.
3. Cache the refiner and interpolation tables. Evaluate positions with
   `Far::PrimvarRefiner::Interpolate()`, vertex/varying data with the matching
   interpolation rule, and each face-varying channel with
   `InterpolateFaceVarying()`. Point-only animation should reevaluate primvars
   without rebuilding refined topology.
4. Emit last-level faces into the renderer's triangle representation while
   retaining a refined-face-to-coarse-face map. Use that map to preserve Hydra
   geometric-subset material assignment.
5. Generate smooth normals on the refined topology. A later limit-stencil path
   may calculate analytic limit normals, but coarse faceted normals must never
   be used as subdivision shading normals.

Implemented details: final vertices and normals are evaluated with OpenSubdiv
limit masks, so the current result is stronger than the original smooth-normal
minimum. Refiner, stencil-table, refined-topology, and coarse-face-map caching
keeps point-only animation topology-invariant.

## Phase 2: topology and primvar correctness

Support and test all information carried by `PxOsdMeshTopology` and
`PxOsdSubdivTags`:

- crease and corner sharpness, including fractional and infinite sharpness;
- holes and left/right-handed orientation;
- vertex boundary and face-varying linear-interpolation rules;
- vertex, varying, and face-varying primvars, including indexed UV seams;
- material subsets after refinement;
- Catmull-Clark, Loop, bilinear, and the `none` polygonal scheme;
- animated points and dirty-bit changes to topology, tags, and refine level.

Focused USD regression scenes should cover levels 0, 1, and 2, a creased cube,
a holed surface, a Loop triangle mesh, a UV seam, and multiple material subsets.
Tests should assert refined counts and primvar values before adding image
comparisons, so failures identify topology problems rather than only pixels.

## Phase 3: realtime evaluation and instancing

Move cached stencil evaluation to Vulkan compute while keeping topology
refinement and cache construction on the CPU. Store refined positions, normals,
and face-varying data in reusable GPU buffers; point animation then dispatches
stencils and updates/refits acceleration structures instead of rebuilding the
entire flattened scene.

At the same time, change the current flattened acceleration structure to one
BLAS per refined prototype plus native TLAS instances. PointInstancer copies can
then share both subdivision data and BLAS storage.

## Phase 4: adaptive limit surfaces

After uniform refinement is correct and profiled, add cached OpenSubdiv patch
tables and limit stencils. Camera-dependent edge factors must be shared between
adjacent patches to remain crack-free. Evaluate patches in compute or mesh
shaders and rebuild/refit only affected BLAS data. Retain uniform refinement as
the deterministic fallback for unsupported hardware and regression testing.

## Initial acceptance criteria

- A Catmull-Clark mesh visibly differs from its coarse cage and matches the
  expected level-1/level-2 topology.
- Creases, corners, holes, UV seams, and material subsets survive refinement.
- Animated points reuse cached topology and interpolation tables.
- Instanced subdivision prototypes produce identical shading while sharing
  refined geometry resources.
- No private OpenUSD symbols or Houdini libraries enter the standalone build.

## Runtime controls

- `enableSubdivision` / `HDCODEX_ENABLE_SUBDIVISION`: defaults to enabled;
  disabling it preserves the authored coarse cage.
- `subdivisionLevel` / `HDCODEX_SUBDIVISION_LEVEL`: defaults to 2 and accepts
  levels 0–8. Level 0 is the coarse cage even when subdivision is enabled.

The fixed level is deliberately renderer-owned and deterministic. Adaptive,
camera-dependent refinement remains Phase 4.
