#pragma once

#include "hdcodex/core/versioned_scene.h"

#include "pxr/imaging/hd/renderDelegate.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdCodexRenderParam final : public HdRenderParam {
public:
    explicit HdCodexRenderParam(hdcodex::VersionedScene* scene) : _scene(scene) {}

    void MarkSceneDirty() noexcept { (void)_scene->MarkDirty(); }
    [[nodiscard]] hdcodex::VersionedScene* GetScene() const noexcept { return _scene; }

private:
    hdcodex::VersionedScene* _scene;
};

PXR_NAMESPACE_CLOSE_SCOPE

