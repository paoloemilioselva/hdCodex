#pragma once

#include "api.h"

#include "pxr/imaging/hd/instancer.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/tf/hashmap.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/value.h"

#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE

/// Renderer-owned Hydra instancer. It flattens all nested transform levels for
/// a prototype while keeping the implementation independent of in-tree plugins.
class HDCODEX_API HdCodexInstancer final : public HdInstancer {
public:
    HdCodexInstancer(HdSceneDelegate* delegate, const SdfPath& id);
    ~HdCodexInstancer() override;

    void Sync(HdSceneDelegate* sceneDelegate,
              HdRenderParam* renderParam,
              HdDirtyBits* dirtyBits) override;

    [[nodiscard]] VtMatrix4dArray ComputeInstanceTransforms(
        const SdfPath& prototypeId) const;

private:
    mutable std::mutex _mutex;
    TfHashMap<TfToken, VtValue, TfToken::HashFunctor> _primvars;
    bool _visible{true};
};

PXR_NAMESPACE_CLOSE_SCOPE
