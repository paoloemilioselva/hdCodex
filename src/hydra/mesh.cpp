#include "mesh.h"

#include "instancer.h"
#include "render_param.h"

#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3h.h"

#include <array>
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
    GfMatrix4d transform;
    VtVec2fArray texcoords;
    VtIntArray texcoordIndices;
    HdInterpolation texcoordInterpolation = HdInterpolationConstant;
    VtVec3fArray normals;
    VtIntArray normalIndices;
    HdInterpolation normalInterpolation = HdInterpolationConstant;
    bool visible = true;
    SdfPath materialId;
    SdfPath instancerId;
    {
        const std::scoped_lock lock(_mutex);
        if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
            const VtValue pointsValue = GetPoints(sceneDelegate);
            if (pointsValue.IsHolding<VtVec3fArray>()) {
                _points = pointsValue.UncheckedGet<VtVec3fArray>();
                changed = true;
            }
        }
        if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id)) {
            _topology = GetMeshTopology(sceneDelegate);
            changed = true;
        }
        if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, id)) {
            if (const auto coordinates = ReadTextureCoordinates(sceneDelegate, id)) {
                _texcoords = coordinates->values;
                _texcoordIndices = coordinates->indices;
                _texcoordInterpolation = coordinates->interpolation;
            } else {
                _texcoords.clear();
                _texcoordIndices.clear();
                _texcoordInterpolation = HdInterpolationConstant;
            }
            changed = true;
        }
        if ((*dirtyBits & HdChangeTracker::DirtyNormals) != 0 ||
            HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, id)) {
            if (const auto authoredNormals = ReadNormals(sceneDelegate, id)) {
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
        texcoords = _texcoords;
        texcoordIndices = _texcoordIndices;
        texcoordInterpolation = _texcoordInterpolation;
        normals = _normals;
        normalIndices = _normalIndices;
        normalInterpolation = _normalInterpolation;
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

            VtVec3iArray triangles;
            VtIntArray primitiveParams;
            HdMeshUtil(&topology, id).ComputeTriangleIndices(
                &triangles, &primitiveParams);

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
            const VtVec3fArray cornerNormals = normals.empty()
                ? VtVec3fArray{}
                : TriangulateNormals(
                    topology, id, triangles, primitiveParams, normals,
                    normalIndices, normalInterpolation);

            hdcodex::SceneMesh mesh;
            mesh.id = id.GetString();
            mesh.materialId = materialId.GetString();
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
