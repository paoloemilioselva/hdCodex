#include "material.h"

#include "render_param.h"

#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"

#if defined(HDCODEX_HAS_MATERIALX)
#include "pxr/imaging/hdMtlx/hdMtlx.h"
#endif

PXR_NAMESPACE_OPEN_SCOPE

#if defined(HDCODEX_HAS_MATERIALX)
namespace {

HdMaterialNetwork2 ToNetwork2(const VtValue& resource)
{
    if (resource.IsHolding<HdMaterialNetworkMap>()) {
        return HdConvertToHdMaterialNetwork2(
            resource.UncheckedGet<HdMaterialNetworkMap>());
    }
    if (resource.IsHolding<HdMaterialNetwork2>()) {
        return resource.UncheckedGet<HdMaterialNetwork2>();
    }
    return {};
}

float FloatParameter(const HdMaterialNode2& node, const char* name, float fallback)
{
    const auto found = node.parameters.find(TfToken(name));
    if (found == node.parameters.end()) return fallback;
    const VtValue& value = found->second;
    if (value.IsHolding<float>()) return value.UncheckedGet<float>();
    if (value.IsHolding<double>()) return static_cast<float>(value.UncheckedGet<double>());
    return fallback;
}

std::array<float, 3> ColorParameter(
    const HdMaterialNode2& node, const char* name, std::array<float, 3> fallback)
{
    const auto found = node.parameters.find(TfToken(name));
    if (found == node.parameters.end()) return fallback;
    const VtValue& value = found->second;
    if (value.IsHolding<GfVec3f>()) {
        const auto color = value.UncheckedGet<GfVec3f>();
        return {color[0], color[1], color[2]};
    }
    if (value.IsHolding<GfVec3d>()) {
        const auto color = value.UncheckedGet<GfVec3d>();
        return {static_cast<float>(color[0]), static_cast<float>(color[1]),
                static_cast<float>(color[2])};
    }
    if (value.IsHolding<GfVec4f>()) {
        const auto color = value.UncheckedGet<GfVec4f>();
        return {color[0], color[1], color[2]};
    }
    return fallback;
}

hdcodex::SceneMaterial ExtractSceneMaterial(
    const VtValue& resource, const SdfPath& materialPath)
{
    hdcodex::SceneMaterial material;
    material.id = materialPath.GetString();
    const HdMaterialNetwork2 network = ToNetwork2(resource);
    for (const auto& [path, node] : network.nodes) {
        (void)path;
        const std::string type = node.nodeTypeId.GetString();
        if (type.find("standard_surface") != std::string::npos) {
            material.baseColor = ColorParameter(node, "base_color", material.baseColor);
            material.metalness = FloatParameter(node, "metalness", material.metalness);
            material.roughness = FloatParameter(node, "specular_roughness", material.roughness);
            const float emissionWeight = FloatParameter(node, "emission", 0.0F);
            material.emission = ColorParameter(node, "emission_color", material.emission);
            for (float& component : material.emission) component *= emissionWeight;
            material.opacity = FloatParameter(node, "opacity", material.opacity);
            return material;
        }
        if (type.find("UsdPreviewSurface") != std::string::npos) {
            material.baseColor = ColorParameter(node, "diffuseColor", material.baseColor);
            material.metalness = FloatParameter(node, "metallic", material.metalness);
            material.roughness = FloatParameter(node, "roughness", material.roughness);
            material.emission = ColorParameter(node, "emissiveColor", material.emission);
            material.opacity = FloatParameter(node, "opacity", material.opacity);
            return material;
        }
    }
    return material;
}

std::shared_ptr<const hdcodex::MaterialXCompiledShader> CompileMaterialX(
    const VtValue& resource,
    const SdfPath& materialPath,
    hdcodex::MaterialXCompiler* compiler)
{
    if (!compiler) return {};

    const HdMaterialNetwork2 network = ToNetwork2(resource);
    if (network.nodes.empty()) return {};

    const auto terminal = network.terminals.find(HdMaterialTerminalTokens->surface);
    if (terminal == network.terminals.end()) return {};
    const auto node = network.nodes.find(terminal->second.upstreamNode);
    if (node == network.nodes.end()) return {};

    const MaterialX::DocumentPtr document = HdMtlxCreateMtlxDocumentFromHdNetwork(
        network,
        node->second,
        terminal->second.upstreamNode,
        materialPath,
        HdMtlxStdLibraries());
    if (!document) return {};

    return std::make_shared<const hdcodex::MaterialXCompiledShader>(
        compiler->CompileDocument(document, materialPath.GetName()));
}

} // namespace
#endif

HdCodexMaterial::HdCodexMaterial(const SdfPath& id) : HdMaterial(id) {}
HdCodexMaterial::~HdCodexMaterial() = default;

void HdCodexMaterial::Sync(HdSceneDelegate* sceneDelegate,
                           HdRenderParam* renderParam,
                           HdDirtyBits* dirtyBits)
{
    if (*dirtyBits & HdMaterial::DirtyResource) {
        const VtValue resource = sceneDelegate->GetMaterialResource(GetId());
#if defined(HDCODEX_HAS_MATERIALX)
        std::shared_ptr<const hdcodex::MaterialXCompiledShader> compiled;
        if (auto* param = dynamic_cast<HdCodexRenderParam*>(renderParam)) {
            param->GetScene()->UpsertMaterial(ExtractSceneMaterial(resource, GetId()));
            try {
                compiled = CompileMaterialX(resource, GetId(), param->GetMaterialCompiler());
            } catch (const std::exception& error) {
                TF_WARN("hdCodex could not compile MaterialX material %s: %s",
                        GetId().GetText(), error.what());
            }
        }
#endif
        {
            const std::scoped_lock lock(_mutex);
            _network = resource;
#if defined(HDCODEX_HAS_MATERIALX)
            _compiledShader = std::move(compiled);
#endif
        }
        if (auto* param = dynamic_cast<HdCodexRenderParam*>(renderParam)) {
#if !defined(HDCODEX_HAS_MATERIALX)
            param->MarkSceneDirty();
#endif
        }
    }
    *dirtyBits = HdMaterial::Clean;
}

#if defined(HDCODEX_HAS_MATERIALX)
std::shared_ptr<const hdcodex::MaterialXCompiledShader>
HdCodexMaterial::GetCompiledShader() const
{
    const std::scoped_lock lock(_mutex);
    return _compiledShader;
}
#endif

HdDirtyBits HdCodexMaterial::GetInitialDirtyBitsMask() const
{
    return HdMaterial::AllDirty;
}

VtValue HdCodexMaterial::GetNetwork() const
{
    const std::scoped_lock lock(_mutex);
    return _network;
}

PXR_NAMESPACE_CLOSE_SCOPE
