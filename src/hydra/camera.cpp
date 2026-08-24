#include "camera.h"

#include "render_param.h"

PXR_NAMESPACE_OPEN_SCOPE

HdCodexCamera::HdCodexCamera(const SdfPath& id) : HdCamera(id) {}
HdCodexCamera::~HdCodexCamera() = default;

void HdCodexCamera::Sync(HdSceneDelegate* sceneDelegate,
                         HdRenderParam* renderParam,
                         HdDirtyBits* dirtyBits)
{
    HdCamera::Sync(sceneDelegate, renderParam, dirtyBits);
    if (auto* param = dynamic_cast<HdCodexRenderParam*>(renderParam)) {
        param->MarkSceneDirty();
    }
}

PXR_NAMESPACE_CLOSE_SCOPE

