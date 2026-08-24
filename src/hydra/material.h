#pragma once

#include "api.h"

#include "pxr/imaging/hd/material.h"
#include "pxr/base/vt/value.h"

#if defined(HDCODEX_HAS_MATERIALX)
#include "hdcodex/materialx/materialx_compiler.h"
#endif

#include <memory>
#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE

class HDCODEX_API HdCodexMaterial final : public HdMaterial {
public:
    explicit HdCodexMaterial(const SdfPath& id);
    ~HdCodexMaterial() override;

    void Sync(HdSceneDelegate* sceneDelegate,
              HdRenderParam* renderParam,
              HdDirtyBits* dirtyBits) override;
    HdDirtyBits GetInitialDirtyBitsMask() const override;

    [[nodiscard]] VtValue GetNetwork() const;
#if defined(HDCODEX_HAS_MATERIALX)
    [[nodiscard]] std::shared_ptr<const hdcodex::MaterialXCompiledShader>
    GetCompiledShader() const;
#endif

private:
    mutable std::mutex _mutex;
    VtValue _network;
#if defined(HDCODEX_HAS_MATERIALX)
    std::shared_ptr<const hdcodex::MaterialXCompiledShader> _compiledShader;
#endif
};

PXR_NAMESPACE_CLOSE_SCOPE
