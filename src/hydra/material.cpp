#include "material.h"

#include "render_param.h"

#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/tf/diagnostic.h"

#if defined(HDCODEX_HAS_MATERIALX)
#include "pxr/imaging/hdMtlx/hdMtlx.h"
#endif

PXR_NAMESPACE_OPEN_SCOPE

#if defined(HDCODEX_HAS_MATERIALX)
namespace {

std::shared_ptr<const hdcodex::MaterialXCompiledShader> CompileMaterialX(
    const VtValue& resource,
    const SdfPath& materialPath,
    hdcodex::MaterialXCompiler* compiler)
{
    if (!compiler) return {};

    HdMaterialNetwork2 network;
    if (resource.IsHolding<HdMaterialNetworkMap>()) {
        network = HdConvertToHdMaterialNetwork2(
            resource.UncheckedGet<HdMaterialNetworkMap>());
    } else if (resource.IsHolding<HdMaterialNetwork2>()) {
        network = resource.UncheckedGet<HdMaterialNetwork2>();
    } else {
        return {};
    }

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
            param->MarkSceneDirty();
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
