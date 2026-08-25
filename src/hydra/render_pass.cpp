#include "render_pass.h"

#include "render_buffer.h"

#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/tf/diagnostic.h"

#include <algorithm>
#include <span>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
namespace {

hdcodex::PathTracerCamera MakeCamera(const HdRenderPassStateSharedPtr& state)
{
    const GfMatrix4d view = state->GetWorldToViewMatrix();
    const GfMatrix4d inverseViewProjection =
        (view * state->GetProjectionMatrix()).GetInverse();
    const GfVec3d origin = view.GetInverse().Transform(GfVec3d(0.0));
    const GfVec3d lowerLeft = inverseViewProjection.Transform(GfVec3d(-1.0, -1.0, -1.0));
    const GfVec3d lowerRight = inverseViewProjection.Transform(GfVec3d(1.0, -1.0, -1.0));
    const GfVec3d upperLeft = inverseViewProjection.Transform(GfVec3d(-1.0, 1.0, -1.0));
    const GfVec3d horizontal = lowerRight - lowerLeft;
    const GfVec3d vertical = upperLeft - lowerLeft;
    return {
        .origin = {static_cast<float>(origin[0]), static_cast<float>(origin[1]),
                   static_cast<float>(origin[2])},
        .lowerLeft = {static_cast<float>(lowerLeft[0]), static_cast<float>(lowerLeft[1]),
                      static_cast<float>(lowerLeft[2])},
        .horizontal = {static_cast<float>(horizontal[0]), static_cast<float>(horizontal[1]),
                       static_cast<float>(horizontal[2])},
        .vertical = {static_cast<float>(vertical[0]), static_cast<float>(vertical[1]),
                     static_cast<float>(vertical[2])},
    };
}

std::vector<float> UpscaleNearest(
    std::span<const float> source,
    unsigned int sourceWidth,
    unsigned int sourceHeight,
    unsigned int width,
    unsigned int height)
{
    std::vector<float> result(static_cast<std::size_t>(width) * height * 4U);
    for (unsigned int y = 0; y < height; ++y) {
        const unsigned int sourceY = std::min(
            sourceHeight - 1U, y * sourceHeight / height);
        for (unsigned int x = 0; x < width; ++x) {
            const unsigned int sourceX = std::min(
                sourceWidth - 1U, x * sourceWidth / width);
            const std::size_t sourceOffset =
                (static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) * 4U;
            const std::size_t destinationOffset =
                (static_cast<std::size_t>(y) * width + x) * 4U;
            std::copy_n(source.data() + sourceOffset, 4U,
                        result.data() + destinationOffset);
        }
    }
    return result;
}

} // namespace

HdCodexRenderPass::HdCodexRenderPass(HdRenderIndex* index,
                                     const HdRprimCollection& collection,
                                     HdRenderDelegate* renderDelegate,
                                     hdcodex::VersionedScene* scene,
                                     hdcodex::VulkanPathTracer* pathTracer)
    : HdRenderPass(index, collection), _renderDelegate(renderDelegate),
      _scene(scene), _pathTracer(pathTracer)
{
}

HdCodexRenderPass::~HdCodexRenderPass() = default;

bool HdCodexRenderPass::IsConverged() const
{
    return _converged && _lastRevision == _scene->PublishedRevision();
}

void HdCodexRenderPass::_Execute(const HdRenderPassStateSharedPtr& renderPassState,
                                 const TfTokenVector& /*renderTags*/)
{
    const unsigned int settingsVersion = _renderDelegate
        ? _renderDelegate->GetRenderSettingsVersion() : 0U;
    const unsigned int targetSamples = static_cast<unsigned int>(std::clamp(
        _renderDelegate
            ? _renderDelegate->GetRenderSetting<int>(
                TfToken("samplesPerPixel"), 128)
            : 128,
        1, 4096));
    const unsigned int productionBounces = static_cast<unsigned int>(std::clamp(
        _renderDelegate
            ? _renderDelegate->GetRenderSetting<int>(TfToken("maxBounces"), 8)
            : 8,
        1, 12));
    const unsigned int samplesPerUpdate = static_cast<unsigned int>(std::clamp(
        _renderDelegate
            ? _renderDelegate->GetRenderSetting<int>(
                TfToken("samplesPerUpdate"), 8)
            : 8,
        1, 64));
    const auto revision = _scene->PublishedRevision();
    const hdcodex::PathTracerCamera camera = MakeCamera(renderPassState);
    HdCodexRenderBuffer* colorBuffer = nullptr;
    const HdRenderPassAovBinding* colorBinding = nullptr;
    for (const HdRenderPassAovBinding& binding : renderPassState->GetAovBindings()) {
        auto* buffer = dynamic_cast<HdCodexRenderBuffer*>(binding.renderBuffer);
        if (!buffer) continue;
        buffer->SetConverged(false);
        if (binding.aovName == HdAovTokens->color) {
            colorBuffer = buffer;
            colorBinding = &binding;
        } else if (_lastRevision != revision) {
            buffer->Clear(binding.clearValue);
        }
    }

    if (!colorBuffer || !_pathTracer) {
        for (const HdRenderPassAovBinding& binding : renderPassState->GetAovBindings()) {
            if (auto* buffer = dynamic_cast<HdCodexRenderBuffer*>(binding.renderBuffer)) {
                buffer->SetConverged(true);
            }
        }
        _converged = true;
        _lastRevision = revision;
        return;
    }

    const unsigned int width = colorBuffer->GetWidth();
    const unsigned int height = colorBuffer->GetHeight();
    const bool cameraChanged = _hasCamera && !(camera == _lastCamera);
    const bool reset = _lastRevision != revision || !_hasCamera ||
        _lastSettingsVersion != settingsVersion ||
        cameraChanged || width != _lastWidth || height != _lastHeight;
    if (reset) {
        try {
            if (_lastRevision != revision) _pathTracer->SetScene(_scene->Snapshot());
            _sampleIndex = 0;
        } catch (const std::exception& error) {
            TF_WARN("hdCodex failed to build the Vulkan path-tracing scene: %s", error.what());
            colorBuffer->Clear(colorBinding->clearValue);
        }
    }

    const bool interactive = cameraChanged && _lastRevision == revision &&
        width == _lastWidth && height == _lastHeight;
    if (_pathTracer->HasGeometry() && width > 0 && height > 0 && interactive) {
        try {
            constexpr unsigned int interactiveScale = 2U;
            constexpr unsigned int interactiveBounces = 2U;
            const unsigned int traceWidth = std::max(
                1U, (width + interactiveScale - 1U) / interactiveScale);
            const unsigned int traceHeight = std::max(
                1U, (height + interactiveScale - 1U) / interactiveScale);
            colorBuffer->WriteFloat4(UpscaleNearest(
                _pathTracer->Render(camera, traceWidth, traceHeight, 0U,
                                    interactiveBounces),
                traceWidth, traceHeight, width, height));
        } catch (const std::exception& error) {
            TF_WARN("hdCodex interactive Vulkan path trace failed: %s", error.what());
            colorBuffer->Clear(colorBinding->clearValue);
        }
    } else if (_pathTracer->HasGeometry() && width > 0 && height > 0 &&
               _sampleIndex < targetSamples) {
        try {
            const unsigned int sampleCount = std::min(
                samplesPerUpdate, targetSamples - _sampleIndex);
            colorBuffer->WriteFloat4(
                _pathTracer->Render(camera, width, height, _sampleIndex,
                                    productionBounces, sampleCount));
            _sampleIndex += sampleCount;
        } catch (const std::exception& error) {
            TF_WARN("hdCodex Vulkan path trace failed: %s", error.what());
            colorBuffer->Clear(colorBinding->clearValue);
            _sampleIndex = targetSamples;
        }
    } else if (!_pathTracer->HasGeometry()) {
        colorBuffer->Clear(colorBinding->clearValue);
        _sampleIndex = targetSamples;
    }

    _converged = _sampleIndex >= targetSamples;
    for (const HdRenderPassAovBinding& binding : renderPassState->GetAovBindings()) {
        if (auto* buffer = dynamic_cast<HdCodexRenderBuffer*>(binding.renderBuffer)) {
            buffer->SetConverged(_converged);
        }
    }
    _lastCamera = camera;
    _lastWidth = width;
    _lastHeight = height;
    _hasCamera = true;
    _lastRevision = revision;
    _lastSettingsVersion = settingsVersion;
}

PXR_NAMESPACE_CLOSE_SCOPE
