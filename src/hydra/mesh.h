#pragma once

#include "api.h"

#include "pxr/imaging/hd/mesh.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/value.h"

#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE

class HDCODEX_API HdCodexMesh final : public HdMesh {
public:
    explicit HdCodexMesh(const SdfPath& id);
    ~HdCodexMesh() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate* sceneDelegate,
              HdRenderParam* renderParam,
              HdDirtyBits* dirtyBits,
              const TfToken& reprToken) override;

protected:
    void _InitRepr(const TfToken& reprToken, HdDirtyBits* dirtyBits) override;
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override;

private:
    mutable std::mutex _mutex;
    VtVec3fArray _points;
    HdMeshTopology _topology;
    VtVec2fArray _texcoords;
    VtIntArray _texcoordIndices;
    HdInterpolation _texcoordInterpolation{HdInterpolationConstant};
    VtVec3fArray _normals;
    VtIntArray _normalIndices;
    HdInterpolation _normalInterpolation{HdInterpolationConstant};
    GfMatrix4d _transform{1.0};
    bool _visible{true};
};

PXR_NAMESPACE_CLOSE_SCOPE
