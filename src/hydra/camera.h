#pragma once

#include "api.h"

#include "pxr/imaging/hd/camera.h"

PXR_NAMESPACE_OPEN_SCOPE

class HDCODEX_API HdCodexCamera final : public HdCamera {
public:
    explicit HdCodexCamera(const SdfPath& id);
    ~HdCodexCamera() override;

    void Sync(HdSceneDelegate* sceneDelegate,
              HdRenderParam* renderParam,
              HdDirtyBits* dirtyBits) override;
};

PXR_NAMESPACE_CLOSE_SCOPE

