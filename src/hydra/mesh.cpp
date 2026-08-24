#include "mesh.h"

#include "instancer.h"
#include "render_param.h"

#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/vec2f.h"

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

            hdcodex::SceneMesh mesh;
            mesh.id = id.GetString();
            mesh.materialId = materialId.GetString();
            mesh.positions.reserve(points.size() * 3U * instanceTransforms.size());
            mesh.indices.reserve(triangles.size() * 3U * instanceTransforms.size());
            mesh.texcoords.reserve(cornerTexcoords.size() * instanceTransforms.size());
            for (const GfMatrix4d& instanceTransform : instanceTransforms) {
                const std::uint32_t vertexBase = static_cast<std::uint32_t>(
                    mesh.positions.size() / 3U);
                const GfMatrix4d worldTransform = transform * instanceTransform;
                for (const GfVec3f& point : points) {
                    const GfVec3d world = worldTransform.Transform(GfVec3d(point));
                    mesh.positions.push_back(static_cast<float>(world[0]));
                    mesh.positions.push_back(static_cast<float>(world[1]));
                    mesh.positions.push_back(static_cast<float>(world[2]));
                }
                for (const GfVec3i& triangle : triangles) {
                    bool valid = true;
                    for (int component = 0; component < 3; ++component) {
                        valid = valid && triangle[component] >= 0 &&
                            static_cast<std::size_t>(triangle[component]) < points.size();
                    }
                    if (valid) {
                        for (int component = 0; component < 3; ++component) {
                            mesh.indices.push_back(vertexBase +
                                static_cast<std::uint32_t>(triangle[component]));
                        }
                    }
                }
                mesh.texcoords.insert(mesh.texcoords.end(),
                    cornerTexcoords.begin(), cornerTexcoords.end());
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
