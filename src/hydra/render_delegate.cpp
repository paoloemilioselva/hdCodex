#include "render_delegate.h"

#include "camera.h"
#include "instancer.h"
#include "light.h"
#include "material.h"
#include "mesh.h"
#include "render_buffer.h"
#include "render_param.h"
#include "render_pass.h"

#if defined(HDCODEX_HAS_MATERIALX)
#include "hdcodex/materialx/materialx_compiler.h"
#endif

#include "pxr/base/gf/vec4f.h"
#include "pxr/imaging/hd/extComputation.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <cstdlib>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE
namespace {

const TfTokenVector SupportedRprims = {HdPrimTypeTokens->mesh};
const TfTokenVector SupportedSprims = {
    HdPrimTypeTokens->camera,
    HdPrimTypeTokens->material,
    HdPrimTypeTokens->domeLight,
    HdPrimTypeTokens->rectLight,
    HdPrimTypeTokens->extComputation,
};
const TfTokenVector SupportedBprims = {HdPrimTypeTokens->renderBuffer};

int EnvironmentInteger(const char* name, int fallback, int minimum, int maximum)
{
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (!end || *end != '\0') return fallback;
    return std::clamp(static_cast<int>(parsed), minimum, maximum);
}

std::string EnvironmentString(const char* name, const char* fallback)
{
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

} // namespace

HdCodexRenderDelegate::HdCodexRenderDelegate() { Initialize({}); }

HdCodexRenderDelegate::HdCodexRenderDelegate(const HdRenderSettingsMap& settingsMap)
{
    Initialize(settingsMap);
}

HdCodexRenderDelegate::~HdCodexRenderDelegate() = default;

void HdCodexRenderDelegate::Initialize(const HdRenderSettingsMap& settingsMap)
{
    HdRenderDelegate::SetRenderSetting(
        TfToken("samplesPerPixel"), VtValue(EnvironmentInteger(
            "HDCODEX_SAMPLES_PER_PIXEL", 128, 1, 4096)));
    HdRenderDelegate::SetRenderSetting(
        TfToken("maxBounces"), VtValue(EnvironmentInteger(
            "HDCODEX_MAX_BOUNCES", 8, 1, 12)));
    HdRenderDelegate::SetRenderSetting(
        TfToken("samplesPerUpdate"), VtValue(EnvironmentInteger(
            "HDCODEX_SAMPLES_PER_UPDATE", 8, 1, 64)));
    HdRenderDelegate::SetRenderSetting(
        TfToken("shadingMode"), VtValue(EnvironmentString(
            "HDCODEX_SHADING_MODE", "fused")));
    for (const auto& [key, value] : settingsMap) {
        HdRenderDelegate::SetRenderSetting(key, value);
    }
#if defined(HDCODEX_HAS_VULKAN)
    _vulkan = std::make_unique<hdcodex::VulkanContext>();
#endif
    _shaderCache = std::make_unique<hdcodex::ShaderCache>(HDCODEX_SHADER_CACHE_DIR);
#if defined(HDCODEX_HAS_MATERIALX)
    _materialCompiler = std::make_unique<hdcodex::MaterialXCompiler>(*_shaderCache);
#endif
    
#if defined(HDCODEX_HAS_VULKAN)
    _pathTracer = std::make_unique<hdcodex::VulkanPathTracer>(*_vulkan, *_shaderCache);
#endif
    const auto shadingMode = hdcodex::ParseShadingMode(
        GetRenderSetting<std::string>(TfToken("shadingMode"), "fused"));
    _renderParam = std::make_unique<HdCodexRenderParam>(
        &_scene, _materialCompiler.get(),
        shadingMode.value_or(hdcodex::ShadingMode::Fused));
    _resourceRegistry = std::make_shared<HdResourceRegistry>();
}

const TfTokenVector& HdCodexRenderDelegate::GetSupportedRprimTypes() const { return SupportedRprims; }
const TfTokenVector& HdCodexRenderDelegate::GetSupportedSprimTypes() const { return SupportedSprims; }
const TfTokenVector& HdCodexRenderDelegate::GetSupportedBprimTypes() const { return SupportedBprims; }
TfTokenVector HdCodexRenderDelegate::GetShaderSourceTypes() const { return {TfToken("mtlx")}; }
TfTokenVector HdCodexRenderDelegate::GetMaterialRenderContexts() const { return {TfToken("mtlx"), TfToken()}; }
TfToken HdCodexRenderDelegate::GetMaterialBindingPurpose() const { return HdTokens->full; }
HdRenderParam* HdCodexRenderDelegate::GetRenderParam() const { return _renderParam.get(); }
HdResourceRegistrySharedPtr HdCodexRenderDelegate::GetResourceRegistry() const { return _resourceRegistry; }

void HdCodexRenderDelegate::SetRenderSetting(
    const TfToken& key, const VtValue& value)
{
    HdRenderDelegate::SetRenderSetting(key, value);
    if (key != TfToken("shadingMode") || !_renderParam) return;
    const std::string name =
        VtValue::Cast<std::string>(value).GetWithDefault(std::string("fused"));
    if (const auto mode = hdcodex::ParseShadingMode(name)) {
        _renderParam->SetShadingMode(*mode);
    }
}

HdRenderPassSharedPtr HdCodexRenderDelegate::CreateRenderPass(
    HdRenderIndex* index, const HdRprimCollection& collection)
{
    return std::make_shared<HdCodexRenderPass>(
        index, collection, this, &_scene,
#if defined(HDCODEX_HAS_VULKAN)
        _pathTracer.get()
#else
        nullptr
#endif
    );
}

HdInstancer* HdCodexRenderDelegate::CreateInstancer(
    HdSceneDelegate* delegate, const SdfPath& id)
{
    return new HdCodexInstancer(delegate, id);
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
    if (typeId == HdPrimTypeTokens->extComputation) {
        return new HdExtComputation(sprimId);
    }
    if (typeId == HdPrimTypeTokens->domeLight ||
        typeId == HdPrimTypeTokens->rectLight) {
        return new HdCodexLight(sprimId, typeId);
    }
    return nullptr;
}

HdSprim* HdCodexRenderDelegate::CreateFallbackSprim(const TfToken& typeId)
{
    return CreateSprim(typeId, SdfPath::EmptyPath());
}

void HdCodexRenderDelegate::DestroySprim(HdSprim* sprim)
{
    if (dynamic_cast<HdCodexMaterial*>(sprim)) {
        _scene.RemoveMaterial(sprim->GetId().GetString());
    }
    if (dynamic_cast<HdCodexLight*>(sprim)) {
        _scene.RemoveLight(sprim->GetId().GetString());
    }
    delete sprim;
}

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

HdRenderSettingDescriptorList
HdCodexRenderDelegate::GetRenderSettingDescriptors() const
{
    return {
        {"Samples per Pixel", TfToken("samplesPerPixel"), VtValue(128)},
        {"Maximum Bounces", TfToken("maxBounces"), VtValue(8)},
        {"Samples per Update", TfToken("samplesPerUpdate"), VtValue(8)},
        {"Shading Mode", TfToken("shadingMode"), VtValue(std::string("fused"))},
    };
}

PXR_NAMESPACE_CLOSE_SCOPE
