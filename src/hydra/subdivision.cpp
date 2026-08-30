#include "subdivision.h"

#include "pxr/imaging/pxOsd/refinerFactory.h"
#include "pxr/imaging/pxOsd/tokens.h"

#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/stencilTable.h>
#include <opensubdiv/far/stencilTableFactory.h>
#include <opensubdiv/far/topologyLevel.h>
#include <opensubdiv/far/topologyRefiner.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <utility>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

struct HdCodexSubdivisionCacheData {
    HdMeshTopology sourceTopology;
    VtIntArray faceVaryingTopology;
    int refinementLevel{0};
    PxOsdTopologyRefinerSharedPtr refiner;
    std::unique_ptr<const OpenSubdiv::Far::StencilTable> vertexStencils;
    std::unique_ptr<const OpenSubdiv::Far::StencilTable> varyingStencils;
    std::unique_ptr<const OpenSubdiv::Far::StencilTable> faceVaryingStencils;
    HdMeshTopology refinedTopology;
    VtIntArray coarseFaceIndices;
    VtIntArray refinedFaceVaryingIndices;
};

HdCodexSubdivisionCache::HdCodexSubdivisionCache() = default;
HdCodexSubdivisionCache::~HdCodexSubdivisionCache() = default;
HdCodexSubdivisionCache::HdCodexSubdivisionCache(
    HdCodexSubdivisionCache&&) noexcept = default;
HdCodexSubdivisionCache& HdCodexSubdivisionCache::operator=(
    HdCodexSubdivisionCache&&) noexcept = default;

void HdCodexSubdivisionCache::Reset()
{
    _data.reset();
}

std::size_t HdCodexSubdivisionCache::GetBuildCount() const noexcept
{
    return _buildCount;
}

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
std::vector<Weighted> ApplyStencils(
    const OpenSubdiv::Far::StencilTable& stencils,
    const std::vector<Weighted>& source)
{
    std::vector<Weighted> result(
        static_cast<std::size_t>(stencils.GetNumStencils()));
    stencils.UpdateValues(source, result);
    return result;
}

std::vector<WeightedVec2> RefineFaceVaryingValues(
    const OpenSubdiv::Far::TopologyRefiner& refiner,
    const OpenSubdiv::Far::StencilTable& stencils,
    const std::vector<WeightedVec2>& source)
{
    OpenSubdiv::Far::PrimvarRefiner primvarRefiner(refiner);
    std::vector<WeightedVec2> current = ApplyStencils(stencils, source);
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

std::unique_ptr<const OpenSubdiv::Far::StencilTable> CreateStencilTable(
    const OpenSubdiv::Far::TopologyRefiner& refiner,
    OpenSubdiv::Far::StencilTableFactory::Mode mode,
    int refinementLevel)
{
    OpenSubdiv::Far::StencilTableFactory::Options options;
    options.interpolationMode = mode;
    options.generateControlVerts = false;
    options.generateIntermediateLevels = false;
    options.factorizeIntermediateLevels = true;
    options.maxLevel = static_cast<unsigned int>(refinementLevel);
    options.fvarChannel = 0;
    return std::unique_ptr<const OpenSubdiv::Far::StencilTable>(
        OpenSubdiv::Far::StencilTableFactory::Create(refiner, options));
}

bool CacheMatches(
    const HdCodexSubdivisionCacheData& data,
    const HdMeshTopology& topology,
    const VtIntArray& faceVaryingTopology,
    int refinementLevel)
{
    return data.refinementLevel == refinementLevel &&
        data.sourceTopology == topology &&
        data.faceVaryingTopology == faceVaryingTopology;
}

} // namespace

bool HdCodexBuildSubdivisionCache(
    const HdMeshTopology& topology,
    const VtIntArray& faceVaryingTopology,
    int refinementLevel,
    HdCodexSubdivisionCache* cache,
    std::string* error)
{
    if (!cache) return Fail(error, "subdivision cache is null");

    std::vector<VtIntArray> faceVaryingTopologies;
    if (!faceVaryingTopology.empty()) {
        faceVaryingTopologies.push_back(faceVaryingTopology);
    }
    PxOsdTopologyRefinerSharedPtr refiner = faceVaryingTopologies.empty()
        ? PxOsdRefinerFactory::Create(topology.GetPxOsdMeshTopology())
        : PxOsdRefinerFactory::Create(
            topology.GetPxOsdMeshTopology(), faceVaryingTopologies);
    if (!refiner) return Fail(error, "OpenSubdiv rejected the mesh topology");

    OpenSubdiv::Far::TopologyRefiner::UniformOptions options(refinementLevel);
    options.fullTopologyInLastLevel = true;
    refiner->RefineUniform(options);

    auto data = std::make_unique<HdCodexSubdivisionCacheData>();
    data->sourceTopology = topology;
    data->faceVaryingTopology = faceVaryingTopology;
    data->refinementLevel = refinementLevel;
    data->refiner = std::move(refiner);
    data->vertexStencils = CreateStencilTable(
        *data->refiner,
        OpenSubdiv::Far::StencilTableFactory::INTERPOLATE_VERTEX,
        refinementLevel);
    data->varyingStencils = CreateStencilTable(
        *data->refiner,
        OpenSubdiv::Far::StencilTableFactory::INTERPOLATE_VARYING,
        refinementLevel);
    if (!faceVaryingTopology.empty()) {
        data->faceVaryingStencils = CreateStencilTable(
            *data->refiner,
            OpenSubdiv::Far::StencilTableFactory::INTERPOLATE_FACE_VARYING,
            refinementLevel);
    }
    if (!data->vertexStencils || !data->varyingStencils ||
        (!faceVaryingTopology.empty() && !data->faceVaryingStencils)) {
        return Fail(error, "OpenSubdiv could not build interpolation stencils");
    }

    const OpenSubdiv::Far::TopologyLevel& finalLevel =
        data->refiner->GetLevel(refinementLevel);
    VtIntArray faceCounts;
    VtIntArray faceIndices;
    for (int face = 0; face < finalLevel.GetNumFaces(); ++face) {
        if (finalLevel.IsFaceHole(face)) continue;
        const OpenSubdiv::Far::ConstIndexArray vertices =
            finalLevel.GetFaceVertices(face);
        if (vertices.size() < 3) continue;
        const int coarseFace = FindCoarseFace(
            *data->refiner, refinementLevel, face);
        if (coarseFace < 0) {
            return Fail(error, "failed to map a refined face to its coarse face");
        }
        faceCounts.push_back(vertices.size());
        data->coarseFaceIndices.push_back(coarseFace);
        for (int corner = 0; corner < vertices.size(); ++corner) {
            faceIndices.push_back(vertices[corner]);
        }
        if (!faceVaryingTopology.empty()) {
            const OpenSubdiv::Far::ConstIndexArray values =
                finalLevel.GetFaceFVarValues(face, 0);
            if (values.size() != vertices.size()) {
                return Fail(error, "refined texture topology is inconsistent");
            }
            for (int corner = 0; corner < values.size(); ++corner) {
                data->refinedFaceVaryingIndices.push_back(values[corner]);
            }
        }
    }
    data->refinedTopology = HdMeshTopology(
        PxOsdOpenSubdivTokens->none,
        topology.GetOrientation(), faceCounts, faceIndices, 0);

    cache->_data = std::move(data);
    ++cache->_buildCount;
    return true;
}

bool HdCodexRefineMesh(
    const HdMeshTopology& topology,
    const VtVec3fArray& points,
    const VtVec2fArray& texcoords,
    const VtIntArray& texcoordIndices,
    HdInterpolation texcoordInterpolation,
    int refinementLevel,
    HdCodexRefinedMeshGeometry* result,
    std::string* error,
    HdCodexSubdivisionCache* cache)
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
    VtIntArray faceVaryingTopology;
    if (!texcoords.empty() && texcoordInterpolation == HdInterpolationFaceVarying) {
        if (texcoordIndices.empty()) {
            if (texcoords.size() != cornerCount) {
                return Fail(error,
                    "face-varying texture coordinates do not match face corners");
            }
            faceVaryingTopology.resize(cornerCount);
            std::iota(
                faceVaryingTopology.begin(), faceVaryingTopology.end(), 0);
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
            faceVaryingTopology = texcoordIndices;
        }
        faceVaryingTexcoords = texcoords;
    }

    HdCodexSubdivisionCache temporaryCache;
    HdCodexSubdivisionCache* activeCache = cache ? cache : &temporaryCache;
    if (!activeCache->_data || !CacheMatches(
            *activeCache->_data, topology, faceVaryingTopology,
            refinementLevel)) {
        if (!HdCodexBuildSubdivisionCache(
                topology, faceVaryingTopology, refinementLevel,
                activeCache, error)) {
            return false;
        }
    }
    const HdCodexSubdivisionCacheData& data = *activeCache->_data;

    const std::vector<WeightedVec3> refinedPositions = ApplyStencils(
        *data.vertexStencils, ToWeighted<WeightedVec3>(points));
    LimitSurfaceValues limitSurface =
        EvaluateLimitSurface(*data.refiner, refinedPositions);
    result->points = FromWeightedVec3(limitSurface.positions);
    result->normals = std::move(limitSurface.normals);
    result->topology = data.refinedTopology;
    result->coarseFaceIndices = data.coarseFaceIndices;

    std::vector<WeightedVec2> refinedTexcoords;
    if (!texcoords.empty()) {
        switch (texcoordInterpolation) {
        case HdInterpolationFaceVarying:
            refinedTexcoords = RefineFaceVaryingValues(
                *data.refiner, *data.faceVaryingStencils,
                ToWeighted<WeightedVec2>(faceVaryingTexcoords));
            break;
        case HdInterpolationVertex:
        case HdInterpolationVarying: {
            const VtVec2fArray flattened =
                FlattenIndexedTexcoords(texcoords, texcoordIndices);
            if (flattened.size() != points.size()) {
                return Fail(error,
                    "vertex texture coordinates do not match the point count");
            }
            const OpenSubdiv::Far::StencilTable& stencils =
                texcoordInterpolation == HdInterpolationVarying
                ? *data.varyingStencils : *data.vertexStencils;
            refinedTexcoords = ApplyStencils(
                stencils, ToWeighted<WeightedVec2>(flattened));
            break;
        }
        case HdInterpolationUniform:
        case HdInterpolationConstant:
            break;
        default:
            return Fail(error, "unsupported texture-coordinate interpolation");
        }
    }

    VtVec2fArray finalFaceVaryingTexcoords;
    if (!refinedTexcoords.empty() &&
        texcoordInterpolation == HdInterpolationFaceVarying) {
        finalFaceVaryingTexcoords.reserve(
            data.refinedFaceVaryingIndices.size());
        for (const int value : data.refinedFaceVaryingIndices) {
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
