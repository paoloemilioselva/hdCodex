#include "subdivision.h"

#include "pxr/imaging/pxOsd/refinerFactory.h"
#include "pxr/imaging/pxOsd/tokens.h"

#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/topologyLevel.h>
#include <opensubdiv/far/topologyRefiner.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <utility>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
namespace {

template <std::size_t Size>
struct WeightedValue {
    float values[Size]{};

    void Clear()
    {
        for (float& value : values) value = 0.0F;
    }

    void AddWithWeight(const WeightedValue& source, float weight)
    {
        for (std::size_t component = 0; component < Size; ++component) {
            values[component] += source.values[component] * weight;
        }
    }
};

using WeightedVec2 = WeightedValue<2>;
using WeightedVec3 = WeightedValue<3>;

bool Fail(std::string* error, std::string message)
{
    if (error) *error = std::move(message);
    return false;
}

std::size_t FaceCornerCount(const HdMeshTopology& topology)
{
    std::size_t result = 0;
    for (const int count : topology.GetFaceVertexCounts()) {
        if (count > 0) result += static_cast<std::size_t>(count);
    }
    return result;
}

VtVec2fArray FlattenIndexedTexcoords(
    const VtVec2fArray& values, const VtIntArray& indices)
{
    if (indices.empty()) return values;
    VtVec2fArray result;
    result.reserve(indices.size());
    for (const int index : indices) {
        if (index < 0 || static_cast<std::size_t>(index) >= values.size()) {
            return {};
        }
        result.push_back(values[static_cast<std::size_t>(index)]);
    }
    return result;
}

template <typename Weighted, typename Source>
std::vector<Weighted> ToWeighted(const Source& source)
{
    std::vector<Weighted> result(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        for (std::size_t component = 0;
             component < std::size(result[index].values); ++component) {
            result[index].values[component] = source[index][component];
        }
    }
    return result;
}

VtVec3fArray FromWeightedVec3(const std::vector<WeightedVec3>& source)
{
    VtVec3fArray result;
    result.reserve(source.size());
    for (const WeightedVec3& value : source) {
        result.push_back(GfVec3f(
            value.values[0], value.values[1], value.values[2]));
    }
    return result;
}

VtVec2fArray FromWeightedVec2(const std::vector<WeightedVec2>& source)
{
    VtVec2fArray result;
    result.reserve(source.size());
    for (const WeightedVec2& value : source) {
        result.push_back(GfVec2f(value.values[0], value.values[1]));
    }
    return result;
}

struct LimitSurfaceValues {
    std::vector<WeightedVec3> positions;
    VtVec3fArray normals;
};

LimitSurfaceValues EvaluateLimitSurface(
    const OpenSubdiv::Far::TopologyRefiner& refiner,
    const std::vector<WeightedVec3>& refinedPositions)
{
    OpenSubdiv::Far::PrimvarRefiner primvarRefiner(refiner);
    LimitSurfaceValues result;
    result.positions.resize(refinedPositions.size());
    std::vector<WeightedVec3> tangent1(refinedPositions.size());
    std::vector<WeightedVec3> tangent2(refinedPositions.size());
    primvarRefiner.Limit(
        refinedPositions, result.positions, tangent1, tangent2);
    result.normals.reserve(refinedPositions.size());
    for (std::size_t index = 0; index < refinedPositions.size(); ++index) {
        const GfVec3f first(
            tangent1[index].values[0],
            tangent1[index].values[1],
            tangent1[index].values[2]);
        const GfVec3f second(
            tangent2[index].values[0],
            tangent2[index].values[1],
            tangent2[index].values[2]);
        GfVec3f normal = GfCross(first, second);
        if (!std::isfinite(normal[0]) || !std::isfinite(normal[1]) ||
            !std::isfinite(normal[2]) || normal.Normalize() <= 1e-12F) {
            normal = GfVec3f(0.0F);
        }
        result.normals.push_back(normal);
    }
    return result;
}

template <typename Weighted>
std::vector<Weighted> RefineVertexValues(
    const OpenSubdiv::Far::TopologyRefiner& refiner,
    const std::vector<Weighted>& source,
    int refinementLevel,
    HdInterpolation interpolation)
{
    OpenSubdiv::Far::PrimvarRefiner primvarRefiner(refiner);
    std::vector<Weighted> current = source;
    for (int level = 1; level <= refinementLevel; ++level) {
        std::vector<Weighted> next(static_cast<std::size_t>(
            refiner.GetLevel(level).GetNumVertices()));
        if (interpolation == HdInterpolationVarying) {
            primvarRefiner.InterpolateVarying(level, current, next);
        } else {
            primvarRefiner.Interpolate(level, current, next);
        }
        current = std::move(next);
    }
    return current;
}

std::vector<WeightedVec2> RefineFaceVaryingValues(
    const OpenSubdiv::Far::TopologyRefiner& refiner,
    const std::vector<WeightedVec2>& source,
    int refinementLevel)
{
    OpenSubdiv::Far::PrimvarRefiner primvarRefiner(refiner);
    std::vector<WeightedVec2> current = source;
    for (int level = 1; level <= refinementLevel; ++level) {
        std::vector<WeightedVec2> next(static_cast<std::size_t>(
            refiner.GetLevel(level).GetNumFVarValues(0)));
        primvarRefiner.InterpolateFaceVarying(level, current, next, 0);
        current = std::move(next);
    }
    std::vector<WeightedVec2> limit(current.size());
    primvarRefiner.LimitFaceVarying(current, limit, 0);
    return limit;
}

int FindCoarseFace(
    const OpenSubdiv::Far::TopologyRefiner& refiner,
    int refinementLevel,
    int face)
{
    for (int level = refinementLevel; level > 0; --level) {
        face = refiner.GetLevel(level).GetFaceParentFace(face);
        if (face < 0) return -1;
    }
    return face;
}

} // namespace

bool HdCodexRefineMesh(
    const HdMeshTopology& topology,
    const VtVec3fArray& points,
    const VtVec2fArray& texcoords,
    const VtIntArray& texcoordIndices,
    HdInterpolation texcoordInterpolation,
    int refinementLevel,
    HdCodexRefinedMeshGeometry* result,
    std::string* error)
{
    if (!result) return Fail(error, "subdivision output is null");
    *result = {};
    if (refinementLevel < 1 || refinementLevel > 8) {
        return Fail(error, "subdivision level must be in the range 1..8");
    }
    if (points.empty()) return Fail(error, "subdivision mesh has no points");

    const TfToken scheme = topology.GetScheme();
    if (scheme == PxOsdOpenSubdivTokens->none) {
        return Fail(error, "polygonal scheme 'none' cannot be subdivided");
    }
    if (scheme != PxOsdOpenSubdivTokens->catmullClark &&
        scheme != PxOsdOpenSubdivTokens->loop &&
        scheme != PxOsdOpenSubdivTokens->bilinear) {
        return Fail(error, "unsupported subdivision scheme: " + scheme.GetString());
    }

    const std::size_t cornerCount = FaceCornerCount(topology);
    if (cornerCount != topology.GetFaceVertexIndices().size()) {
        return Fail(error, "face counts do not match the face-index array");
    }
    for (const int index : topology.GetFaceVertexIndices()) {
        if (index < 0 || static_cast<std::size_t>(index) >= points.size()) {
            return Fail(error, "face index is outside the point array");
        }
    }

    VtVec2fArray faceVaryingTexcoords;
    std::vector<VtIntArray> faceVaryingTopologies;
    if (!texcoords.empty() && texcoordInterpolation == HdInterpolationFaceVarying) {
        VtIntArray topologyIndices;
        if (texcoordIndices.empty()) {
            if (texcoords.size() != cornerCount) {
                return Fail(error,
                    "face-varying texture coordinates do not match face corners");
            }
            topologyIndices.resize(cornerCount);
            std::iota(topologyIndices.begin(), topologyIndices.end(), 0);
        } else {
            if (texcoordIndices.size() != cornerCount) {
                return Fail(error,
                    "face-varying texture indices do not match face corners");
            }
            for (const int index : texcoordIndices) {
                if (index < 0 ||
                    static_cast<std::size_t>(index) >= texcoords.size()) {
                    return Fail(error,
                        "face-varying texture index is outside the value array");
                }
            }
            topologyIndices = texcoordIndices;
        }
        faceVaryingTexcoords = texcoords;
        faceVaryingTopologies.push_back(std::move(topologyIndices));
    }

    PxOsdTopologyRefinerSharedPtr refiner = faceVaryingTopologies.empty()
        ? PxOsdRefinerFactory::Create(topology.GetPxOsdMeshTopology())
        : PxOsdRefinerFactory::Create(
            topology.GetPxOsdMeshTopology(), faceVaryingTopologies);
    if (!refiner) return Fail(error, "OpenSubdiv rejected the mesh topology");

    OpenSubdiv::Far::TopologyRefiner::UniformOptions options(refinementLevel);
    options.fullTopologyInLastLevel = true;
    refiner->RefineUniform(options);

    const std::vector<WeightedVec3> refinedPositions = RefineVertexValues(
        *refiner, ToWeighted<WeightedVec3>(points), refinementLevel,
        HdInterpolationVertex);
    LimitSurfaceValues limitSurface =
        EvaluateLimitSurface(*refiner, refinedPositions);
    result->points = FromWeightedVec3(limitSurface.positions);
    result->normals = std::move(limitSurface.normals);

    std::vector<WeightedVec2> refinedTexcoords;
    if (!texcoords.empty()) {
        switch (texcoordInterpolation) {
        case HdInterpolationFaceVarying:
            refinedTexcoords = RefineFaceVaryingValues(
                *refiner, ToWeighted<WeightedVec2>(faceVaryingTexcoords),
                refinementLevel);
            break;
        case HdInterpolationVertex:
        case HdInterpolationVarying: {
            const VtVec2fArray flattened =
                FlattenIndexedTexcoords(texcoords, texcoordIndices);
            if (flattened.size() != points.size()) {
                return Fail(error,
                    "vertex texture coordinates do not match the point count");
            }
            refinedTexcoords = RefineVertexValues(
                *refiner, ToWeighted<WeightedVec2>(flattened), refinementLevel,
                texcoordInterpolation);
            break;
        }
        case HdInterpolationUniform:
        case HdInterpolationConstant:
            break;
        default:
            return Fail(error, "unsupported texture-coordinate interpolation");
        }
    }

    const OpenSubdiv::Far::TopologyLevel& finalLevel =
        refiner->GetLevel(refinementLevel);
    VtIntArray faceCounts;
    VtIntArray faceIndices;
    VtVec2fArray finalFaceVaryingTexcoords;
    for (int face = 0; face < finalLevel.GetNumFaces(); ++face) {
        if (finalLevel.IsFaceHole(face)) continue;
        const OpenSubdiv::Far::ConstIndexArray vertices =
            finalLevel.GetFaceVertices(face);
        if (vertices.size() < 3) continue;
        const int coarseFace = FindCoarseFace(*refiner, refinementLevel, face);
        if (coarseFace < 0) {
            return Fail(error, "failed to map a refined face to its coarse face");
        }
        faceCounts.push_back(vertices.size());
        result->coarseFaceIndices.push_back(coarseFace);
        for (int corner = 0; corner < vertices.size(); ++corner) {
            faceIndices.push_back(vertices[corner]);
        }
        if (!refinedTexcoords.empty() &&
            texcoordInterpolation == HdInterpolationFaceVarying) {
            const OpenSubdiv::Far::ConstIndexArray values =
                finalLevel.GetFaceFVarValues(face, 0);
            if (values.size() != vertices.size()) {
                return Fail(error, "refined texture topology is inconsistent");
            }
            for (int corner = 0; corner < values.size(); ++corner) {
                const int value = values[corner];
                if (value < 0 ||
                    static_cast<std::size_t>(value) >= refinedTexcoords.size()) {
                    return Fail(error, "refined texture index is invalid");
                }
                const WeightedVec2& coordinate =
                    refinedTexcoords[static_cast<std::size_t>(value)];
                finalFaceVaryingTexcoords.push_back(GfVec2f(
                    coordinate.values[0], coordinate.values[1]));
            }
        }
    }

    result->topology = HdMeshTopology(
        PxOsdOpenSubdivTokens->none,
        topology.GetOrientation(), faceCounts, faceIndices, 0);

    if (texcoords.empty()) return true;
    result->texcoordInterpolation = texcoordInterpolation;
    switch (texcoordInterpolation) {
    case HdInterpolationFaceVarying:
        result->texcoords = std::move(finalFaceVaryingTexcoords);
        break;
    case HdInterpolationVertex:
    case HdInterpolationVarying:
        result->texcoords = FromWeightedVec2(refinedTexcoords);
        break;
    case HdInterpolationUniform: {
        const VtVec2fArray flattened =
            FlattenIndexedTexcoords(texcoords, texcoordIndices);
        if (flattened.size() != topology.GetFaceVertexCounts().size()) {
            return Fail(error,
                "uniform texture coordinates do not match the coarse faces");
        }
        result->texcoords.reserve(result->coarseFaceIndices.size());
        for (const int face : result->coarseFaceIndices) {
            result->texcoords.push_back(flattened[static_cast<std::size_t>(face)]);
        }
        break;
    }
    case HdInterpolationConstant:
        result->texcoords.push_back(texcoords.front());
        break;
    default:
        break;
    }
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
