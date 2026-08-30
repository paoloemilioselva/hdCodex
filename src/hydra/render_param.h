#pragma once

#include "hdcodex/core/versioned_scene.h"
#include "hdcodex/core/shading_mode.h"

#include "pxr/imaging/hd/renderDelegate.h"

#include <atomic>

namespace hdcodex {
class MaterialXCompiler;
}

PXR_NAMESPACE_OPEN_SCOPE

class HdCodexRenderParam final : public HdRenderParam {
public:
    HdCodexRenderParam(
        hdcodex::VersionedScene* scene,
        hdcodex::MaterialXCompiler* materialCompiler,
        hdcodex::ShadingMode shadingMode,
        bool subdivisionEnabled,
        int subdivisionLevel)
        : _scene(scene), _materialCompiler(materialCompiler),
          _shadingMode(shadingMode),
          _subdivisionEnabled(subdivisionEnabled),
          _subdivisionLevel(subdivisionLevel) {}

    void MarkSceneDirty() noexcept { (void)_scene->MarkDirty(); }
    [[nodiscard]] hdcodex::VersionedScene* GetScene() const noexcept { return _scene; }
    [[nodiscard]] hdcodex::MaterialXCompiler* GetMaterialCompiler() const noexcept
    {
        return _materialCompiler;
    }
    void SetShadingMode(hdcodex::ShadingMode mode) noexcept
    {
        _shadingMode.store(mode, std::memory_order_release);
    }
    [[nodiscard]] hdcodex::ShadingMode GetShadingMode() const noexcept
    {
        return _shadingMode.load(std::memory_order_acquire);
    }
    bool SetSubdivisionEnabled(bool enabled) noexcept
    {
        return _subdivisionEnabled.exchange(
            enabled, std::memory_order_acq_rel) != enabled;
    }
    [[nodiscard]] bool IsSubdivisionEnabled() const noexcept
    {
        return _subdivisionEnabled.load(std::memory_order_acquire);
    }
    bool SetSubdivisionLevel(int level) noexcept
    {
        return _subdivisionLevel.exchange(
            level, std::memory_order_acq_rel) != level;
    }
    [[nodiscard]] int GetSubdivisionLevel() const noexcept
    {
        return _subdivisionLevel.load(std::memory_order_acquire);
    }

private:
    hdcodex::VersionedScene* _scene;
    hdcodex::MaterialXCompiler* _materialCompiler;
    std::atomic<hdcodex::ShadingMode> _shadingMode;
    std::atomic<bool> _subdivisionEnabled;
    std::atomic<int> _subdivisionLevel;
};

PXR_NAMESPACE_CLOSE_SCOPE
