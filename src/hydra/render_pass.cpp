#include "render_pass.h"

#include "render_buffer.h"

#include "pxr/imaging/hd/renderPassState.h"

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

HdCodexRenderPass::HdCodexRenderPass(HdRenderIndex* index,
                                     const HdRprimCollection& collection,
                                     hdcodex::VersionedScene* scene)
    : HdRenderPass(index, collection), _scene(scene)
{
}

HdCodexRenderPass::~HdCodexRenderPass() = default;

bool HdCodexRenderPass::IsConverged() const
{
    return _lastRevision == _scene->PublishedRevision();
}

void HdCodexRenderPass::_Execute(const HdRenderPassStateSharedPtr& renderPassState,
                                 const TfTokenVector& /*renderTags*/)
{
    const auto revision = _scene->PublishedRevision();
    for (const HdRenderPassAovBinding& binding : renderPassState->GetAovBindings()) {
        auto* buffer = dynamic_cast<HdCodexRenderBuffer*>(binding.renderBuffer);
        if (!buffer) {
            continue;
        }
        buffer->SetConverged(false);
        if (_lastRevision != revision) {
            buffer->Clear(binding.clearValue);
        }
        buffer->SetConverged(true);
    }
    _lastRevision = revision;
}

PXR_NAMESPACE_CLOSE_SCOPE

