#pragma once

#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/array.h"
#include "pxr/imaging/hd/enums.h"
#include "pxr/imaging/hd/meshTopology.h"

#include <cstddef>
#include <memory>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

struct HdCodexRefinedMeshGeometry {
    HdMeshTopology topology;
    VtVec3fArray points;
    VtVec3fArray normals;
    VtVec2fArray texcoords;
    VtIntArray texcoordIndices;
    HdInterpolation texcoordInterpolation{HdInterpolationConstant};
    VtIntArray coarseFaceIndices;
};

struct HdCodexSubdivisionCacheData;

// Owns topology-dependent OpenSubdiv refinement and interpolation tables.
// Reusing this object lets animated points and primvar values be evaluated
// without rebuilding topology or stencil weights.
class HdCodexSubdivisionCache {
public:
    HdCodexSubdivisionCache();
    ~HdCodexSubdivisionCache();
    HdCodexSubdivisionCache(HdCodexSubdivisionCache&&) noexcept;
    HdCodexSubdivisionCache& operator=(HdCodexSubdivisionCache&&) noexcept;

    HdCodexSubdivisionCache(const HdCodexSubdivisionCache&) = delete;
    HdCodexSubdivisionCache& operator=(const HdCodexSubdivisionCache&) = delete;

    void Reset();
    [[nodiscard]] std::size_t GetBuildCount() const noexcept;

private:
    std::unique_ptr<HdCodexSubdivisionCacheData> _data;
    std::size_t _buildCount{0};

    friend bool HdCodexRefineMesh(
        const HdMeshTopology&,
        const VtVec3fArray&,
        const VtVec2fArray&,
        const VtIntArray&,
        HdInterpolation,
        int,
        HdCodexRefinedMeshGeometry*,
        std::string*,
        HdCodexSubdivisionCache*);
    friend bool HdCodexBuildSubdivisionCache(
        const HdMeshTopology&,
        const VtIntArray&,
        int,
        HdCodexSubdivisionCache*,
        std::string*);
};

// Uniformly refines a subdivision mesh and its primary texture-coordinate
// primvar, then evaluates final vertices and normals on the limit surface. The
// returned topology contains only the final, non-hole faces and
// coarseFaceIndices maps each of those faces back to its authored coarse face.
// Returns false with an actionable error for invalid or unsupported input.
bool HdCodexRefineMesh(
    const HdMeshTopology& topology,
    const VtVec3fArray& points,
    const VtVec2fArray& texcoords,
    const VtIntArray& texcoordIndices,
    HdInterpolation texcoordInterpolation,
    int refinementLevel,
    HdCodexRefinedMeshGeometry* result,
    std::string* error,
    HdCodexSubdivisionCache* cache = nullptr);

PXR_NAMESPACE_CLOSE_SCOPE
