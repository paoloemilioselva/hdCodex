#pragma once

#include "api.h"

#include "pxr/imaging/hd/material.h"
#include "pxr/base/vt/value.h"

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

private:
    mutable std::mutex _mutex;
    VtValue _network;
};

PXR_NAMESPACE_CLOSE_SCOPE

