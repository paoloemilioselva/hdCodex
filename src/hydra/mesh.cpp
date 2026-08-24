#include "mesh.h"

#include "render_param.h"

#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/sceneDelegate.h"

PXR_NAMESPACE_OPEN_SCOPE

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
    bool changed = false;
    VtVec3fArray points;
    HdMeshTopology topology;
    GfMatrix4d transform;
    bool visible = true;
    SdfPath materialId;
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
        points = _points;
        topology = _topology;
        transform = _transform;
        visible = _visible;
        materialId = GetMaterialId();
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

            hdcodex::SceneMesh mesh;
            mesh.id = id.GetString();
            mesh.materialId = materialId.GetString();
            mesh.positions.reserve(points.size() * 3U);
            for (const GfVec3f& point : points) {
                const GfVec3d world = transform.Transform(GfVec3d(point));
                mesh.positions.push_back(static_cast<float>(world[0]));
                mesh.positions.push_back(static_cast<float>(world[1]));
                mesh.positions.push_back(static_cast<float>(world[2]));
            }
            mesh.indices.reserve(triangles.size() * 3U);
            for (const GfVec3i& triangle : triangles) {
                for (int component = 0; component < 3; ++component) {
                    if (triangle[component] >= 0) {
                        mesh.indices.push_back(
                            static_cast<std::uint32_t>(triangle[component]));
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
