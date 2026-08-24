#include "render_delegate.h"

#include "camera.h"
#include "material.h"
#include "mesh.h"
#include "render_buffer.h"
#include "render_param.h"
#include "render_pass.h"

#if defined(HDCODEX_HAS_MATERIALX)
#include "hdcodex/materialx/materialx_compiler.h"
#endif

#include "pxr/base/gf/vec4f.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/tokens.h"

PXR_NAMESPACE_OPEN_SCOPE
namespace {

const TfTokenVector SupportedRprims = {HdPrimTypeTokens->mesh};
const TfTokenVector SupportedSprims = {
    HdPrimTypeTokens->camera,
    HdPrimTypeTokens->material,
};
const TfTokenVector SupportedBprims = {HdPrimTypeTokens->renderBuffer};

} // namespace

HdCodexRenderDelegate::HdCodexRenderDelegate() { Initialize({}); }

HdCodexRenderDelegate::HdCodexRenderDelegate(const HdRenderSettingsMap& settingsMap)
{
    Initialize(settingsMap);
}

HdCodexRenderDelegate::~HdCodexRenderDelegate() = default;

void HdCodexRenderDelegate::Initialize(const HdRenderSettingsMap& settingsMap)
{
#if defined(HDCODEX_HAS_VULKAN)
    _vulkan = std::make_unique<hdcodex::VulkanContext>();
#endif
    _shaderCache = std::make_unique<hdcodex::ShaderCache>(HDCODEX_SHADER_CACHE_DIR);
#if defined(HDCODEX_HAS_MATERIALX)
    _materialCompiler = std::make_unique<hdcodex::MaterialXCompiler>(*_shaderCache);
#endif
    _renderParam = std::make_unique<HdCodexRenderParam>(&_scene, _materialCompiler.get());
    _resourceRegistry = std::make_shared<HdResourceRegistry>();
    for (const auto& [key, value] : settingsMap) {
        HdRenderDelegate::SetRenderSetting(key, value);
    }
}

const TfTokenVector& HdCodexRenderDelegate::GetSupportedRprimTypes() const { return SupportedRprims; }
const TfTokenVector& HdCodexRenderDelegate::GetSupportedSprimTypes() const { return SupportedSprims; }
const TfTokenVector& HdCodexRenderDelegate::GetSupportedBprimTypes() const { return SupportedBprims; }
TfTokenVector HdCodexRenderDelegate::GetShaderSourceTypes() const { return {TfToken("mtlx")}; }
TfTokenVector HdCodexRenderDelegate::GetMaterialRenderContexts() const { return {TfToken("mtlx"), TfToken()}; }
TfToken HdCodexRenderDelegate::GetMaterialBindingPurpose() const { return HdTokens->full; }
HdRenderParam* HdCodexRenderDelegate::GetRenderParam() const { return _renderParam.get(); }
HdResourceRegistrySharedPtr HdCodexRenderDelegate::GetResourceRegistry() const { return _resourceRegistry; }

HdRenderPassSharedPtr HdCodexRenderDelegate::CreateRenderPass(
    HdRenderIndex* index, const HdRprimCollection& collection)
{
    return std::make_shared<HdCodexRenderPass>(index, collection, &_scene);
}

HdInstancer* HdCodexRenderDelegate::CreateInstancer(
    HdSceneDelegate* delegate, const SdfPath& id)
{
    return new HdInstancer(delegate, id);
}

void HdCodexRenderDelegate::DestroyInstancer(HdInstancer* instancer) { delete instancer; }

HdRprim* HdCodexRenderDelegate::CreateRprim(const TfToken& typeId, const SdfPath& rprimId)
{
    return typeId == HdPrimTypeTokens->mesh ? new HdCodexMesh(rprimId) : nullptr;
}

void HdCodexRenderDelegate::DestroyRprim(HdRprim* rprim)
{
    if (rprim) _scene.RemoveMesh(rprim->GetId().GetString());
    delete rprim;
}

HdSprim* HdCodexRenderDelegate::CreateSprim(const TfToken& typeId, const SdfPath& sprimId)
{
    if (typeId == HdPrimTypeTokens->camera) {
        return new HdCodexCamera(sprimId);
    }
    if (typeId == HdPrimTypeTokens->material) {
        return new HdCodexMaterial(sprimId);
    }
    return nullptr;
}

HdSprim* HdCodexRenderDelegate::CreateFallbackSprim(const TfToken& typeId)
{
    return CreateSprim(typeId, SdfPath::EmptyPath());
}

void HdCodexRenderDelegate::DestroySprim(HdSprim* sprim) { delete sprim; }

HdBprim* HdCodexRenderDelegate::CreateBprim(const TfToken& typeId, const SdfPath& bprimId)
{
    return typeId == HdPrimTypeTokens->renderBuffer ? new HdCodexRenderBuffer(bprimId) : nullptr;
}

HdBprim* HdCodexRenderDelegate::CreateFallbackBprim(const TfToken& typeId)
{
    return CreateBprim(typeId, SdfPath::EmptyPath());
}

void HdCodexRenderDelegate::DestroyBprim(HdBprim* bprim) { delete bprim; }

void HdCodexRenderDelegate::CommitResources(HdChangeTracker* /*tracker*/)
{
    (void)_scene.Publish();
}

HdAovDescriptor HdCodexRenderDelegate::GetDefaultAovDescriptor(const TfToken& name) const
{
    if (name == HdAovTokens->color) {
        return {HdFormatFloat32Vec4, true, VtValue(GfVec4f(0.0F, 0.0F, 0.0F, 1.0F))};
    }
    if (name == HdAovTokens->depth) {
        return {HdFormatFloat32, false, VtValue(1.0F)};
    }
    return {};
}

PXR_NAMESPACE_CLOSE_SCOPE
