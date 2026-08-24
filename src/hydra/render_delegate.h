#pragma once

#include "api.h"

#include "hdcodex/core/versioned_scene.h"
#include "hdcodex/core/shader_cache.h"
#if defined(HDCODEX_HAS_VULKAN)
#include "hdcodex/gpu/vulkan_context.h"
#include "hdcodex/gpu/vulkan_path_tracer.h"
#endif
#include "pxr/imaging/hd/renderDelegate.h"

#include <memory>

namespace hdcodex {
class MaterialXCompiler;
}

PXR_NAMESPACE_OPEN_SCOPE

class HdCodexRenderParam;

class HDCODEX_API HdCodexRenderDelegate final : public HdRenderDelegate {
public:
    HdCodexRenderDelegate();
    explicit HdCodexRenderDelegate(const HdRenderSettingsMap& settingsMap);
    ~HdCodexRenderDelegate() override;

    const TfTokenVector& GetSupportedRprimTypes() const override;
    const TfTokenVector& GetSupportedSprimTypes() const override;
    const TfTokenVector& GetSupportedBprimTypes() const override;
    TfTokenVector GetShaderSourceTypes() const override;
    TfTokenVector GetMaterialRenderContexts() const override;
    TfToken GetMaterialBindingPurpose() const override;
    HdRenderParam* GetRenderParam() const override;
    HdResourceRegistrySharedPtr GetResourceRegistry() const override;
    HdRenderPassSharedPtr CreateRenderPass(
        HdRenderIndex* index, const HdRprimCollection& collection) override;
    HdInstancer* CreateInstancer(HdSceneDelegate* delegate, const SdfPath& id) override;
    void DestroyInstancer(HdInstancer* instancer) override;
    HdRprim* CreateRprim(const TfToken& typeId, const SdfPath& rprimId) override;
    void DestroyRprim(HdRprim* rprim) override;
    HdSprim* CreateSprim(const TfToken& typeId, const SdfPath& sprimId) override;
    HdSprim* CreateFallbackSprim(const TfToken& typeId) override;
    void DestroySprim(HdSprim* sprim) override;
    HdBprim* CreateBprim(const TfToken& typeId, const SdfPath& bprimId) override;
    HdBprim* CreateFallbackBprim(const TfToken& typeId) override;
    void DestroyBprim(HdBprim* bprim) override;
    void CommitResources(HdChangeTracker* tracker) override;
    HdAovDescriptor GetDefaultAovDescriptor(const TfToken& name) const override;

private:
    void Initialize(const HdRenderSettingsMap& settingsMap);

    hdcodex::VersionedScene _scene;
    std::unique_ptr<hdcodex::ShaderCache> _shaderCache;
    std::unique_ptr<hdcodex::MaterialXCompiler> _materialCompiler;
    std::unique_ptr<HdCodexRenderParam> _renderParam;
    HdResourceRegistrySharedPtr _resourceRegistry;
#if defined(HDCODEX_HAS_VULKAN)
    std::unique_ptr<hdcodex::VulkanContext> _vulkan;
    std::unique_ptr<hdcodex::VulkanPathTracer> _pathTracer;
#endif
};

PXR_NAMESPACE_CLOSE_SCOPE
