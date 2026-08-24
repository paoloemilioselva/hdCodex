#include "material.h"

#include "render_param.h"

#include "pxr/imaging/hd/sceneDelegate.h"

PXR_NAMESPACE_OPEN_SCOPE

HdCodexMaterial::HdCodexMaterial(const SdfPath& id) : HdMaterial(id) {}
HdCodexMaterial::~HdCodexMaterial() = default;

void HdCodexMaterial::Sync(HdSceneDelegate* sceneDelegate,
                           HdRenderParam* renderParam,
                           HdDirtyBits* dirtyBits)
{
    if (*dirtyBits & HdMaterial::DirtyResource) {
        {
            const std::scoped_lock lock(_mutex);
            _network = sceneDelegate->GetMaterialResource(GetId());
        }
        if (auto* param = dynamic_cast<HdCodexRenderParam*>(renderParam)) {
            param->MarkSceneDirty();
        }
    }
    *dirtyBits = HdMaterial::Clean;
}

HdDirtyBits HdCodexMaterial::GetInitialDirtyBitsMask() const
{
    return HdMaterial::AllDirty;
}

VtValue HdCodexMaterial::GetNetwork() const
{
    const std::scoped_lock lock(_mutex);
    return _network;
}

PXR_NAMESPACE_CLOSE_SCOPE

