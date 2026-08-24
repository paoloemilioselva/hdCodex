#include "mesh.h"

#include "render_param.h"

#include "pxr/imaging/hd/changeTracker.h"
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
    {
        const std::scoped_lock lock(_mutex);
        if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
            const VtValue points = GetPoints(sceneDelegate);
            if (points.IsHolding<VtVec3fArray>()) {
                _points = points.UncheckedGet<VtVec3fArray>();
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
    }

    _UpdateVisibility(sceneDelegate, dirtyBits);
    UpdateRenderTag(sceneDelegate, renderParam);
    *dirtyBits = HdChangeTracker::Clean;
    if (changed) {
        if (auto* param = dynamic_cast<HdCodexRenderParam*>(renderParam)) {
            param->MarkSceneDirty();
        }
    }
}

void HdCodexMesh::_InitRepr(const TfToken& /*reprToken*/, HdDirtyBits* /*dirtyBits*/) {}

HdDirtyBits HdCodexMesh::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

PXR_NAMESPACE_CLOSE_SCOPE
