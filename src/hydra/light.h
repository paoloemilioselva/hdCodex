#pragma once

#include "api.h"

#include "hdcodex/core/versioned_scene.h"

#include "pxr/imaging/hd/light.h"
#include "pxr/base/gf/matrix4d.h"

PXR_NAMESPACE_OPEN_SCOPE

class HDCODEX_API HdCodexLight final : public HdLight {
public:
    HdCodexLight(const SdfPath& id, const TfToken& lightType);
    ~HdCodexLight() override;

    void Sync(HdSceneDelegate* sceneDelegate,
              HdRenderParam* renderParam,
              HdDirtyBits* dirtyBits) override;
    HdDirtyBits GetInitialDirtyBitsMask() const override;

private:
    void _UpdateGeometry();

    TfToken _lightType;
    GfMatrix4d _transform{1.0};
    GfMatrix4d _domeOffset{1.0};
    float _authoredRadius{0.5F};
    float _authoredLength{1.0F};
    hdcodex::SceneLight _light;
};

PXR_NAMESPACE_CLOSE_SCOPE
