#include "mesh.h"

#include "instancer.h"
#include "render_param.h"
#include "subdivision.h"

#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/extComputationUtils.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/smoothNormals.h"
#include "pxr/imaging/hd/vertexAdjacency.h"
#include "pxr/imaging/pxOsd/tokens.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3h.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <utility>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

struct TextureCoordinates {
    VtVec2fArray values;
    VtIntArray indices;
    HdInterpolation interpolation{HdInterpolationConstant};
};

struct MeshNormals {
    VtVec3fArray values;
    VtIntArray indices;
    HdInterpolation interpolation{HdInterpolationConstant};
};

struct ComputedPrimvars {
    HdExtComputationUtils::ValueStore values;
    std::map<TfToken, HdInterpolation> interpolations;
};

void ApplyTopologicalInvisibility(HdMeshTopology* topology)
{
    if (!topology) return;
    VtIntArray holes = topology->GetHoleIndices();
    holes.insert(
        holes.end(), topology->GetInvisibleFaces().begin(),
        topology->GetInvisibleFaces().end());

    const VtIntArray& invisiblePoints = topology->GetInvisiblePoints();
    if (!invisiblePoints.empty()) {
        const VtIntArray& counts = topology->GetFaceVertexCounts();
        const VtIntArray& indices = topology->GetFaceVertexIndices();
        std::size_t cornerOffset = 0;
        for (std::size_t face = 0; face < counts.size(); ++face) {
            const int count = counts[face];
            bool invisible = false;
            for (int corner = 0; corner < count &&
                 cornerOffset + static_cast<std::size_t>(corner) < indices.size();
                 ++corner) {
                invisible = invisible || std::find(
                    invisiblePoints.begin(), invisiblePoints.end(),
                    indices[cornerOffset + static_cast<std::size_t>(corner)]) !=
                        invisiblePoints.end();
            }
            if (invisible) holes.push_back(static_cast<int>(face));
            if (count > 0) cornerOffset += static_cast<std::size_t>(count);
        }
    }

    std::sort(holes.begin(), holes.end());
    holes.erase(std::unique(holes.begin(), holes.end()), holes.end());
    topology->SetHoleIndices(holes);
}

ComputedPrimvars ReadComputedPrimvars(
    HdSceneDelegate* sceneDelegate, const SdfPath& id)
{
    constexpr std::array interpolations = {
        HdInterpolationFaceVarying,
        HdInterpolationVertex,
        HdInterpolationVarying,
        HdInterpolationUniform,
        HdInterpolationConstant,
    };
    HdExtComputationPrimvarDescriptorVector descriptors;
    ComputedPrimvars result;
    for (const HdInterpolation interpolation : interpolations) {
        HdExtComputationPrimvarDescriptorVector found =
            sceneDelegate->GetExtComputationPrimvarDescriptors(id, interpolation);
        for (const HdExtComputationPrimvarDescriptor& descriptor : found) {
            result.interpolations[descriptor.name] = interpolation;
            descriptors.push_back(descriptor);
        }
    }
    if (!descriptors.empty()) {
        result.values = HdExtComputationUtils::GetComputedPrimvarValues(
            descriptors, sceneDelegate);
    }
    return result;
}

VtVec2fArray ToVec2fArray(const VtValue& value)
{
    if (value.IsHolding<VtVec2fArray>()) {
        return value.UncheckedGet<VtVec2fArray>();
    }
    VtVec2fArray result;
    if (value.IsHolding<VtVec2dArray>()) {
        const auto& source = value.UncheckedGet<VtVec2dArray>();
        result.reserve(source.size());
        for (const GfVec2d& item : source) {
            result.push_back(GfVec2f(
                static_cast<float>(item[0]), static_cast<float>(item[1])));
        }
    }
    return result;
}

VtVec3fArray ToVec3fArray(const VtValue& value)
{
    if (value.IsHolding<VtVec3fArray>()) {
        return value.UncheckedGet<VtVec3fArray>();
    }
    VtVec3fArray result;
    if (value.IsHolding<VtVec3dArray>()) {
        const auto& source = value.UncheckedGet<VtVec3dArray>();
        result.reserve(source.size());
        for (const GfVec3d& item : source) {
            result.push_back(GfVec3f(
                static_cast<float>(item[0]), static_cast<float>(item[1]),
                static_cast<float>(item[2])));
        }
    } else if (value.IsHolding<VtVec3hArray>()) {
        const auto& source = value.UncheckedGet<VtVec3hArray>();
        result.reserve(source.size());
        for (const GfVec3h& item : source) {
            result.push_back(GfVec3f(
                static_cast<float>(item[0]), static_cast<float>(item[1]),
                static_cast<float>(item[2])));
        }
    }
    return result;
}

std::optional<TextureCoordinates> ReadTextureCoordinates(
    HdSceneDelegate* sceneDelegate, const SdfPath& id)
{
    constexpr std::array interpolations = {
        HdInterpolationFaceVarying,
        HdInterpolationVertex,
        HdInterpolationVarying,
        HdInterpolationUniform,
        HdInterpolationConstant,
    };
    static const TfToken stToken("st");
    for (const HdInterpolation interpolation : interpolations) {
        const HdPrimvarDescriptorVector descriptors =
            sceneDelegate->GetPrimvarDescriptors(id, interpolation);
        const HdPrimvarDescriptor* selected = nullptr;
        for (const HdPrimvarDescriptor& descriptor : descriptors) {
            if (descriptor.name == stToken) {
                selected = &descriptor;
                break;
            }
            if (!selected && descriptor.role == HdPrimvarRoleTokens->textureCoordinate) {
                selected = &descriptor;
            }
        }
        if (!selected) continue;

        VtIntArray indices;
        const VtValue value = selected->indexed
            ? sceneDelegate->GetIndexedPrimvar(id, selected->name, &indices)
            : sceneDelegate->Get(id, selected->name);
        VtVec2fArray coordinates = ToVec2fArray(value);
        if (!coordinates.empty()) {
            return TextureCoordinates{
                std::move(coordinates), std::move(indices), interpolation};
        }
    }
    return std::nullopt;
}

std::optional<MeshNormals> ReadNormals(
    HdSceneDelegate* sceneDelegate, const SdfPath& id)
{
    constexpr std::array interpolations = {
        HdInterpolationFaceVarying,
        HdInterpolationVertex,
        HdInterpolationVarying,
        HdInterpolationUniform,
        HdInterpolationConstant,
    };
    for (const HdInterpolation interpolation : interpolations) {
        const HdPrimvarDescriptorVector descriptors =
            sceneDelegate->GetPrimvarDescriptors(id, interpolation);
        for (const HdPrimvarDescriptor& descriptor : descriptors) {
            if (descriptor.name != HdTokens->normals) continue;
            VtIntArray indices;
            const VtValue value = descriptor.indexed
                ? sceneDelegate->GetIndexedPrimvar(id, descriptor.name, &indices)
                : sceneDelegate->Get(id, descriptor.name);
            VtVec3fArray normals = ToVec3fArray(value);
            if (!normals.empty()) {
                return MeshNormals{
                    std::move(normals), std::move(indices), interpolation};
            }
        }
    }
    return std::nullopt;
}

VtVec2fArray FlattenIndexedCoordinates(
    const VtVec2fArray& values, const VtIntArray& indices)
{
    if (indices.empty()) return values;
    VtVec2fArray result;
    result.reserve(indices.size());
    for (const int index : indices) {
        result.push_back(index >= 0 && static_cast<std::size_t>(index) < values.size()
            ? values[static_cast<std::size_t>(index)] : GfVec2f(0.0F));
    }
    return result;
}

VtVec3fArray FlattenIndexedNormals(
    const VtVec3fArray& values, const VtIntArray& indices)
{
    if (indices.empty()) return values;
    VtVec3fArray result;
    result.reserve(indices.size());
    for (const int index : indices) {
        result.push_back(index >= 0 && static_cast<std::size_t>(index) < values.size()
            ? values[static_cast<std::size_t>(index)] : GfVec3f(0.0F));
    }
    return result;
}

std::vector<float> TriangulateTextureCoordinates(
    const HdMeshTopology& topology,
    const SdfPath& id,
    const VtVec3iArray& triangles,
    const VtIntArray& primitiveParams,
    const VtVec2fArray& values,
    const VtIntArray& valueIndices,
    HdInterpolation interpolation)
{
    const VtVec2fArray flattened = FlattenIndexedCoordinates(values, valueIndices);
    VtVec2fArray corners;
    if (interpolation == HdInterpolationFaceVarying) {
        VtValue triangulated;
        const HdMeshComputationResult result = HdMeshUtil(&topology, id)
            .ComputeTriangulatedFaceVaryingPrimvar(
                flattened.cdata(), static_cast<int>(flattened.size()),
                HdTypeFloatVec2, &triangulated);
        if (result == HdMeshComputationResult::Success &&
            triangulated.IsHolding<VtVec2fArray>()) {
            corners = triangulated.UncheckedGet<VtVec2fArray>();
        } else if (result == HdMeshComputationResult::Unchanged) {
            corners = flattened;
        }
    } else {
        corners.reserve(triangles.size() * 3U);
        for (std::size_t triangleIndex = 0; triangleIndex < triangles.size(); ++triangleIndex) {
            const GfVec3i& triangle = triangles[triangleIndex];
            for (int corner = 0; corner < 3; ++corner) {
                std::size_t source = 0;
                if (interpolation == HdInterpolationVertex ||
                    interpolation == HdInterpolationVarying) {
                    source = triangle[corner] >= 0
                        ? static_cast<std::size_t>(triangle[corner]) : 0U;
                } else if (interpolation == HdInterpolationUniform &&
                           triangleIndex < primitiveParams.size()) {
                    source = static_cast<std::size_t>(std::max(
                        HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(
                            primitiveParams[triangleIndex]), 0));
                }
                corners.push_back(source < flattened.size()
                    ? flattened[source] : GfVec2f(0.0F));
            }
        }
    }

    if (corners.size() != triangles.size() * 3U) return {};
    std::vector<float> result;
    result.reserve(corners.size() * 2U);
    for (const GfVec2f& coordinate : corners) {
        result.push_back(coordinate[0]);
        result.push_back(coordinate[1]);
    }
    return result;
}

VtVec3fArray TriangulateNormals(
    const HdMeshTopology& topology,
    const SdfPath& id,
    const VtVec3iArray& triangles,
    const VtIntArray& primitiveParams,
    const VtVec3fArray& values,
    const VtIntArray& valueIndices,
    HdInterpolation interpolation)
{
    const VtVec3fArray flattened = FlattenIndexedNormals(values, valueIndices);
    VtVec3fArray corners;
    if (interpolation == HdInterpolationFaceVarying) {
        VtValue triangulated;
        const HdMeshComputationResult result = HdMeshUtil(&topology, id)
            .ComputeTriangulatedFaceVaryingPrimvar(
                flattened.cdata(), static_cast<int>(flattened.size()),
                HdTypeFloatVec3, &triangulated);
        if (result == HdMeshComputationResult::Success &&
            triangulated.IsHolding<VtVec3fArray>()) {
            corners = triangulated.UncheckedGet<VtVec3fArray>();
        } else if (result == HdMeshComputationResult::Unchanged) {
            corners = flattened;
        }
    } else {
        corners.reserve(triangles.size() * 3U);
        for (std::size_t triangleIndex = 0; triangleIndex < triangles.size(); ++triangleIndex) {
            const GfVec3i& triangle = triangles[triangleIndex];
            for (int corner = 0; corner < 3; ++corner) {
                std::size_t source = 0;
                if (interpolation == HdInterpolationVertex ||
                    interpolation == HdInterpolationVarying) {
                    source = triangle[corner] >= 0
                        ? static_cast<std::size_t>(triangle[corner]) : 0U;
                } else if (interpolation == HdInterpolationUniform &&
                           triangleIndex < primitiveParams.size()) {
                    source = static_cast<std::size_t>(std::max(
                        HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(
                            primitiveParams[triangleIndex]), 0));
                }
                corners.push_back(source < flattened.size()
                    ? flattened[source] : GfVec3f(0.0F));
            }
        }
    }
    return corners.size() == triangles.size() * 3U ? corners : VtVec3fArray{};
}

} // namespace

HdCodexMesh::HdCodexMesh(const SdfPath& id) : HdMesh(id) {}
HdCodexMesh::~HdCodexMesh() = default;

HdDirtyBits HdCodexMesh::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::AllSceneDirtyBits;
}

void HdCodexMesh::Sync(HdSceneDelegate* sceneDelegate,
                       HdRenderParam* renderParam,
                       HdDirtyBits* dirtyBits,
                       const TfToken& /*reprToken*/)
{
    const SdfPath& id = GetId();
    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(
        sceneDelegate->GetRenderIndex(), GetInstancerId());
    bool changed = false;
    VtVec3fArray points;
    HdMeshTopology topology;
    HdDisplayStyle displayStyle;
    GfMatrix4d transform;
    VtVec2fArray texcoords;
    VtIntArray texcoordIndices;
    HdInterpolation texcoordInterpolation = HdInterpolationConstant;
    VtVec3fArray normals;
    VtIntArray normalIndices;
    HdInterpolation normalInterpolation = HdInterpolationConstant;
    GfVec3f displayColor(0.5F);
    bool visible = true;
    SdfPath materialId;
    SdfPath instancerId;
    const bool primvarsDirty = HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, id);
    const bool normalsDirty = (*dirtyBits & HdChangeTracker::DirtyNormals) != 0;
    const ComputedPrimvars computed = (primvarsDirty || normalsDirty)
        ? ReadComputedPrimvars(sceneDelegate, id) : ComputedPrimvars{};
    {
        const std::scoped_lock lock(_mutex);
        if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
            const auto computedPoints = computed.values.find(HdTokens->points);
            VtVec3fArray synchronizedPoints = computedPoints != computed.values.end()
                ? ToVec3fArray(computedPoints->second) : VtVec3fArray{};
            if (synchronizedPoints.empty()) {
                synchronizedPoints = ToVec3fArray(GetPoints(sceneDelegate));
            }
            if (!synchronizedPoints.empty()) {
                _points = std::move(synchronizedPoints);
                changed = true;
            }
        }
        const bool topologyDirty =
            HdChangeTracker::IsTopologyDirty(*dirtyBits, id);
        if (topologyDirty) {
            _topology = GetMeshTopology(sceneDelegate);
            ApplyTopologicalInvisibility(&_topology);
            changed = true;
        }
        if (topologyDirty ||
            (*dirtyBits & HdChangeTracker::DirtySubdivTags) != 0) {
            _topology.SetSubdivTags(GetSubdivTags(sceneDelegate));
            changed = true;
        }
        if ((*dirtyBits & HdChangeTracker::DirtyDisplayStyle) != 0) {
            _displayStyle = GetDisplayStyle(sceneDelegate);
            changed = true;
        }
        if (primvarsDirty) {
            if (const auto coordinates = ReadTextureCoordinates(sceneDelegate, id)) {
                _texcoords = coordinates->values;
                _texcoordIndices = coordinates->indices;
                _texcoordInterpolation = coordinates->interpolation;
            } else {
                _texcoords.clear();
                _texcoordIndices.clear();
                _texcoordInterpolation = HdInterpolationConstant;
            }
            const VtVec3fArray colors = ToVec3fArray(
                sceneDelegate->Get(id, HdTokens->displayColor));
            _displayColor = colors.empty() ? GfVec3f(0.5F) : colors.front();
            changed = true;
        }
        if (normalsDirty || primvarsDirty) {
            const auto computedNormals = computed.values.find(HdTokens->normals);
            const VtVec3fArray synchronizedNormals =
                computedNormals != computed.values.end()
                ? ToVec3fArray(computedNormals->second) : VtVec3fArray{};
            if (!synchronizedNormals.empty()) {
                _normals = synchronizedNormals;
                _normalIndices.clear();
                const auto interpolation =
                    computed.interpolations.find(HdTokens->normals);
                _normalInterpolation = interpolation != computed.interpolations.end()
                    ? interpolation->second : HdInterpolationVertex;
            } else if (const auto authoredNormals = ReadNormals(sceneDelegate, id)) {
                _normals = authoredNormals->values;
                _normalIndices = authoredNormals->indices;
                _normalInterpolation = authoredNormals->interpolation;
            } else {
                _normals.clear();
                _normalIndices.clear();
                _normalInterpolation = HdInterpolationConstant;
            }
            changed = true;
        }
        if (HdChangeTracker::IsTransformDirty(*dirtyBits, id)) {
            _transform = sceneDelegate->GetTransform(id);
            changed = true;
        }
        if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
            _visible = sceneDelegate->GetVisible(id);
            changed = true;
        }
        if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
            SetMaterialId(sceneDelegate->GetMaterialId(id));
            changed = true;
        }
        if (HdChangeTracker::IsInstancerDirty(*dirtyBits, id)) changed = true;
        points = _points;
        topology = _topology;
        displayStyle = _displayStyle;
        texcoords = _texcoords;
        texcoordIndices = _texcoordIndices;
        texcoordInterpolation = _texcoordInterpolation;
        normals = _normals;
        normalIndices = _normalIndices;
        normalInterpolation = _normalInterpolation;
        displayColor = _displayColor;
        transform = _transform;
        visible = _visible;
        materialId = GetMaterialId();
        instancerId = GetInstancerId();
    }

    _UpdateVisibility(sceneDelegate, dirtyBits);
    UpdateRenderTag(sceneDelegate, renderParam);
    *dirtyBits = HdChangeTracker::Clean;
    if (changed) {
        if (auto* param = dynamic_cast<HdCodexRenderParam*>(renderParam)) {
            auto* scene = param->GetScene();
            if (!visible || points.empty()) {
                scene->RemoveMesh(id.GetString());
                return;
            }

            const HdMeshTopology coarseTopology = topology;
            VtIntArray refinedFaceToCoarseFace;
            const int configuredLevel = param->GetSubdivisionLevel();
            const int refinementLevel = configuredLevel >= 0
                ? configuredLevel : displayStyle.refineLevel;
            if (param->IsSubdivisionEnabled() && refinementLevel > 0 &&
                topology.GetScheme() != PxOsdOpenSubdivTokens->none) {
                HdCodexRefinedMeshGeometry refined;
                std::string subdivisionError;
                if (HdCodexRefineMesh(
                    topology, points, texcoords, texcoordIndices,
                    texcoordInterpolation, refinementLevel,
                    &refined, &subdivisionError)) {
                    topology = std::move(refined.topology);
                    points = std::move(refined.points);
                    texcoords = std::move(refined.texcoords);
                    texcoordIndices = std::move(refined.texcoordIndices);
                    texcoordInterpolation = refined.texcoordInterpolation;
                    refinedFaceToCoarseFace =
                        std::move(refined.coarseFaceIndices);
                    // Authored coarse normals do not describe the refined
                    // surface. Use OpenSubdiv limit-surface normals.
                    normals = std::move(refined.normals);
                    normalIndices.clear();
                    normalInterpolation = HdInterpolationVertex;
                } else {
                    TF_WARN("hdCodex could not refine %s: %s",
                        id.GetText(), subdivisionError.c_str());
                }
            }

            VtVec3iArray triangles;
            VtIntArray primitiveParams;
            HdMeshUtil(&topology, id).ComputeTriangleIndices(
                &triangles, &primitiveParams);

            const std::string baseMaterialId = materialId.GetString();
            std::vector<std::string> faceMaterialIds(
                coarseTopology.GetFaceVertexCounts().size(), baseMaterialId);
            for (const HdGeomSubset& subset : coarseTopology.GetGeomSubsets()) {
                if (subset.type != HdGeomSubset::TypeFaceSet ||
                    subset.materialId.IsEmpty()) {
                    continue;
                }
                for (const int face : subset.indices) {
                    if (face >= 0 &&
                        static_cast<std::size_t>(face) < faceMaterialIds.size()) {
                        faceMaterialIds[static_cast<std::size_t>(face)] =
                            subset.materialId.GetString();
                    }
                }
            }
            std::vector<std::string> triangleMaterialIds(
                triangles.size(), baseMaterialId);
            for (std::size_t triangleIndex = 0;
                 triangleIndex < primitiveParams.size() &&
                 triangleIndex < triangleMaterialIds.size(); ++triangleIndex) {
                const int refinedFace =
                    HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(
                    primitiveParams[triangleIndex]);
                const int face = refinedFace >= 0 &&
                    static_cast<std::size_t>(refinedFace) <
                        refinedFaceToCoarseFace.size()
                    ? refinedFaceToCoarseFace[static_cast<std::size_t>(refinedFace)]
                    : refinedFace;
                if (face >= 0 &&
                    static_cast<std::size_t>(face) < faceMaterialIds.size()) {
                    triangleMaterialIds[triangleIndex] =
                        faceMaterialIds[static_cast<std::size_t>(face)];
                }
            }

            VtMatrix4dArray instanceTransforms;
            if (instancerId.IsEmpty()) {
                instanceTransforms.push_back(GfMatrix4d(1.0));
            } else {
                HdInstancer* instancer =
                    sceneDelegate->GetRenderIndex().GetInstancer(instancerId);
                if (auto* codexInstancer = dynamic_cast<HdCodexInstancer*>(instancer)) {
                    instanceTransforms =
                        codexInstancer->ComputeInstanceTransforms(id);
                }
            }
            if (instanceTransforms.empty()) {
                scene->RemoveMesh(id.GetString());
                return;
            }

            const std::vector<float> cornerTexcoords = texcoords.empty()
                ? std::vector<float>{}
                : TriangulateTextureCoordinates(
                    topology, id, triangles, primitiveParams, texcoords,
                    texcoordIndices, texcoordInterpolation);
            if (displayStyle.flatShadingEnabled) {
                normals.clear();
                normalIndices.clear();
                normalInterpolation = HdInterpolationConstant;
            } else if (normals.empty()) {
                Hd_VertexAdjacency adjacency;
                adjacency.BuildAdjacencyTable(&topology);
                normals = Hd_SmoothNormals::ComputeSmoothNormals(
                    &adjacency, static_cast<int>(points.size()), points.cdata());
                normalIndices.clear();
                normalInterpolation = HdInterpolationVertex;
            }
            const VtVec3fArray cornerNormals = TriangulateNormals(
                topology, id, triangles, primitiveParams, normals,
                normalIndices, normalInterpolation);

            hdcodex::SceneMesh mesh;
            mesh.id = id.GetString();
            mesh.materialId = baseMaterialId;
            mesh.displayColor = {
                displayColor[0], displayColor[1], displayColor[2]};
            mesh.positions.reserve(points.size() * 3U * instanceTransforms.size());
            mesh.indices.reserve(triangles.size() * 3U * instanceTransforms.size());
            mesh.texcoords.reserve(cornerTexcoords.size() * instanceTransforms.size());
            mesh.normals.reserve(triangles.size() * 9U * instanceTransforms.size());
            for (const GfMatrix4d& instanceTransform : instanceTransforms) {
                const std::uint32_t vertexBase = static_cast<std::uint32_t>(
                    mesh.positions.size() / 3U);
                const GfMatrix4d worldTransform = transform * instanceTransform;
                const GfMatrix4d normalTransform =
                    worldTransform.GetInverse().GetTranspose();
                for (const GfVec3f& point : points) {
                    const GfVec3d world = worldTransform.Transform(GfVec3d(point));
                    mesh.positions.push_back(static_cast<float>(world[0]));
                    mesh.positions.push_back(static_cast<float>(world[1]));
                    mesh.positions.push_back(static_cast<float>(world[2]));
                }
                for (std::size_t triangleIndex = 0;
                     triangleIndex < triangles.size(); ++triangleIndex) {
                    const GfVec3i& triangle = triangles[triangleIndex];
                    bool valid = true;
                    for (int component = 0; component < 3; ++component) {
                        valid = valid && triangle[component] >= 0 &&
                            static_cast<std::size_t>(triangle[component]) < points.size();
                    }
                    if (valid) {
                        mesh.triangleMaterialIds.push_back(
                            triangleIndex < triangleMaterialIds.size()
                            ? triangleMaterialIds[triangleIndex] : baseMaterialId);
                        for (int component = 0; component < 3; ++component) {
                            mesh.indices.push_back(vertexBase +
                                static_cast<std::uint32_t>(triangle[component]));
                            const std::size_t corner = triangleIndex * 3U +
                                static_cast<std::size_t>(component);
                            const std::size_t uvOffset = corner * 2U;
                            mesh.texcoords.push_back(uvOffset + 1U < cornerTexcoords.size()
                                ? cornerTexcoords[uvOffset] : 0.0F);
                            mesh.texcoords.push_back(uvOffset + 1U < cornerTexcoords.size()
                                ? cornerTexcoords[uvOffset + 1U] : 0.0F);

                            GfVec3d worldNormal(0.0);
                            if (corner < cornerNormals.size()) {
                                worldNormal = normalTransform.TransformDir(
                                    GfVec3d(cornerNormals[corner]));
                                if (worldNormal.Normalize() <= 1e-12) {
                                    worldNormal = GfVec3d(0.0);
                                }
                            }
                            mesh.normals.push_back(static_cast<float>(worldNormal[0]));
                            mesh.normals.push_back(static_cast<float>(worldNormal[1]));
                            mesh.normals.push_back(static_cast<float>(worldNormal[2]));
                        }
                    }
                }
            }
            scene->UpsertMesh(std::move(mesh));
        }
    }
}

void HdCodexMesh::_InitRepr(const TfToken& /*reprToken*/, HdDirtyBits* /*dirtyBits*/) {}

HdDirtyBits HdCodexMesh::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

PXR_NAMESPACE_CLOSE_SCOPE
