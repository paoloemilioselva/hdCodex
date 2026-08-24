#pragma once

#include "api.h"

#include "hdcodex/core/versioned_scene.h"
#include "pxr/imaging/hd/renderPass.h"

PXR_NAMESPACE_OPEN_SCOPE

class HDCODEX_API HdCodexRenderPass final : public HdRenderPass {
public:
    HdCodexRenderPass(HdRenderIndex* index,
                      const HdRprimCollection& collection,
                      hdcodex::VersionedScene* scene);
    ~HdCodexRenderPass() override;

    bool IsConverged() const override;

protected:
    void _Execute(const HdRenderPassStateSharedPtr& renderPassState,
                  const TfTokenVector& renderTags) override;

private:
    hdcodex::VersionedScene* _scene;
    hdcodex::VersionedScene::Revision _lastRevision{~hdcodex::VersionedScene::Revision{0}};
};

PXR_NAMESPACE_CLOSE_SCOPE

