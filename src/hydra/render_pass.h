#pragma once

#include "api.h"

#include "hdcodex/core/versioned_scene.h"
#include "hdcodex/gpu/vulkan_path_tracer.h"
#include "pxr/imaging/hd/renderPass.h"

PXR_NAMESPACE_OPEN_SCOPE

class HDCODEX_API HdCodexRenderPass final : public HdRenderPass {
public:
    HdCodexRenderPass(HdRenderIndex* index,
                      const HdRprimCollection& collection,
                      hdcodex::VersionedScene* scene,
                      hdcodex::VulkanPathTracer* pathTracer);
    ~HdCodexRenderPass() override;

    bool IsConverged() const override;

protected:
    void _Execute(const HdRenderPassStateSharedPtr& renderPassState,
                  const TfTokenVector& renderTags) override;

private:
    hdcodex::VersionedScene* _scene;
    hdcodex::VulkanPathTracer* _pathTracer;
    hdcodex::VersionedScene::Revision _lastRevision{~hdcodex::VersionedScene::Revision{0}};
    hdcodex::PathTracerCamera _lastCamera{};
    unsigned int _lastWidth{0};
    unsigned int _lastHeight{0};
    unsigned int _sampleIndex{0};
    bool _hasCamera{false};
    bool _converged{false};
};

PXR_NAMESPACE_CLOSE_SCOPE
