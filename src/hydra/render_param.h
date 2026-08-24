#pragma once

#include "hdcodex/core/versioned_scene.h"

#include "pxr/imaging/hd/renderDelegate.h"

namespace hdcodex {
class MaterialXCompiler;
}

PXR_NAMESPACE_OPEN_SCOPE

class HdCodexRenderParam final : public HdRenderParam {
public:
    HdCodexRenderParam(
        hdcodex::VersionedScene* scene,
        hdcodex::MaterialXCompiler* materialCompiler)
        : _scene(scene), _materialCompiler(materialCompiler) {}

    void MarkSceneDirty() noexcept { (void)_scene->MarkDirty(); }
    [[nodiscard]] hdcodex::VersionedScene* GetScene() const noexcept { return _scene; }
    [[nodiscard]] hdcodex::MaterialXCompiler* GetMaterialCompiler() const noexcept
    {
        return _materialCompiler;
    }

private:
    hdcodex::VersionedScene* _scene;
    hdcodex::MaterialXCompiler* _materialCompiler;
};

PXR_NAMESPACE_CLOSE_SCOPE
