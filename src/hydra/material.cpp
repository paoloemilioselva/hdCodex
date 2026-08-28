#include "material.h"

#include "render_param.h"
#include "texture_loader.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"

#if defined(HDCODEX_HAS_MATERIALX)
#include "pxr/imaging/hdMtlx/hdMtlx.h"
#endif

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string_view>

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

bool IsUsdNativeShaderId(const TfToken& identifier)
{
    return identifier.GetString().starts_with("Usd");
}

bool IsUsdNativeShaderNetwork(const VtValue& resource)
{
    if (resource.IsHolding<HdMaterialNetworkMap>()) {
        const HdMaterialNetworkMap& map =
            resource.UncheckedGet<HdMaterialNetworkMap>();
        for (const auto& [terminal, network] : map.map) {
            (void)terminal;
            if (std::ranges::any_of(network.nodes, [](const HdMaterialNode& node) {
                    return IsUsdNativeShaderId(node.identifier);
                })) {
                return true;
            }
        }
    }
    const HdMaterialNetwork2 network = ToNetwork2(resource);
    return std::ranges::any_of(network.nodes, [](const auto& entry) {
        return IsUsdNativeShaderId(entry.second.nodeTypeId);
    });
}

std::string LoadTexture(
    hdcodex::VersionedScene* scene,
    std::string_view path,
    std::string_view colorSpace)
{
    const bool srgb = colorSpace == "srgb_texture" ||
        colorSpace == "srgb_rec709_scene";
    return hdcodex::LoadSceneTexture(
        scene, std::string(path),
        srgb ? hdcodex::TextureColorSpace::Srgb
             : hdcodex::TextureColorSpace::Raw,
        false);
}

void AttachGeneratedProgram(
    hdcodex::SceneMaterial& material,
    const hdcodex::MaterialXCompiledShader& compiled,
    hdcodex::VersionedScene* scene)
{
    const auto descriptorKind = [](hdcodex::SpirvDescriptorKind kind) {
        using Source = hdcodex::SpirvDescriptorKind;
        using Target = hdcodex::SceneMaterial::GeneratedDescriptorKind;
        switch (kind) {
        case Source::UniformBuffer: return Target::UniformBuffer;
        case Source::StorageBuffer: return Target::StorageBuffer;
        case Source::SampledImage: return Target::SampledImage;
        case Source::StorageImage: return Target::StorageImage;
        case Source::AccelerationStructure: return Target::AccelerationStructure;
        case Source::Unknown: return Target::Unknown;
        }
        return Target::Unknown;
    };
    const auto copyDescriptors = [&descriptorKind](
        const std::vector<hdcodex::SpirvDescriptor>& source,
        std::vector<hdcodex::SceneMaterial::GeneratedDescriptor>& target) {
        target.reserve(source.size());
        for (const hdcodex::SpirvDescriptor& sourceDescriptor : source) {
            hdcodex::SceneMaterial::GeneratedDescriptor descriptor{
                .name = sourceDescriptor.name,
                .set = sourceDescriptor.set,
                .binding = sourceDescriptor.binding,
                .kind = descriptorKind(sourceDescriptor.kind),
            };
            descriptor.members.reserve(sourceDescriptor.members.size());
            for (const hdcodex::SpirvBlockMember& member :
                 sourceDescriptor.members) {
                descriptor.members.push_back({member.name, member.offset});
            }
            target.push_back(std::move(descriptor));
        }
    };

    material.materialXMode = compiled.mode;
    material.materialXVertexSpirv = compiled.vertexSpirv.words;
    material.materialXPixelSpirv = compiled.pixelSpirv.words;
    copyDescriptors(
        compiled.vertexDescriptors, material.materialXVertexDescriptors);
    copyDescriptors(
        compiled.pixelDescriptors, material.materialXPixelDescriptors);
    material.materialXPublicUniforms.reserve(compiled.publicUniforms.size());
    for (const hdcodex::MaterialXShaderInput& input : compiled.publicUniforms) {
        material.materialXPublicUniforms.push_back({
            .name = input.name,
            .type = input.type,
            .value = input.value,
        });
    }

    std::map<std::string, std::string, std::less<>> closureTextures;
    for (const hdcodex::MaterialXShaderInput& texture : compiled.textures) {
        // Lighting resources are renderer-owned private uniforms, not material
        // graph images. They are bound by the raster backend.
        if (texture.value.empty() || texture.value == "$envRadiance" ||
            texture.value == "$envIrradiance") {
            continue;
        }
        const std::string textureId = LoadTexture(
            scene, texture.value, texture.colorSpace);
        if (textureId.empty()) continue;
        closureTextures.try_emplace(texture.value, textureId);
        material.materialXTextures.push_back({
            .uniformName = texture.name,
            .textureId = textureId,
            .colorSpace = texture.colorSpace,
        });
    }
    const auto resolveClosureTexture = [&closureTextures](std::string& source) {
        if (source.empty()) return;
        const auto found = closureTextures.find(source);
        if (found == closureTextures.end()) {
            throw std::runtime_error(
                "generated MaterialX closure texture was not reflected: " + source);
        }
        source = found->second;
    };
    for (std::string* texture : {
             &material.baseColorTexture, &material.metalnessTexture,
             &material.roughnessTexture, &material.emissionTexture,
             &material.opacityTexture, &material.normalTexture,
             &material.transmissionTexture, &material.specularTexture,
             &material.specularColorTexture, &material.coatTexture,
             &material.coatColorTexture, &material.coatRoughnessTexture,
             &material.subsurfaceTexture, &material.subsurfaceColorTexture,
             &material.subsurfaceRadiusTexture}) {
        resolveClosureTexture(*texture);
    }

    material.materialXOutputNode = compiled.program.outputNode;
    material.materialXProgram.reserve(compiled.program.nodes.size());
    for (const hdcodex::MaterialXProgramNode& sourceNode : compiled.program.nodes) {
        hdcodex::SceneMaterial::GeneratedNode node{
            .name = sourceNode.name,
            .category = sourceNode.category,
            .nodeDef = sourceNode.nodeDef,
            .type = sourceNode.type,
        };
        node.inputs.reserve(sourceNode.inputs.size());
        for (const hdcodex::MaterialXProgramInput& sourceInput : sourceNode.inputs) {
            node.inputs.push_back({
                .name = sourceInput.name,
                .type = sourceInput.type,
                .value = sourceInput.value,
                .upstreamNode = sourceInput.upstreamNode,
                .upstreamOutput = sourceInput.upstreamOutput,
            });
        }
        material.materialXProgram.push_back(std::move(node));
    }
}

std::shared_ptr<const hdcodex::MaterialXCompiledShader> CompileMaterialX(
    const VtValue& resource,
    const SdfPath& materialPath,
    hdcodex::MaterialXCompiler* compiler,
    hdcodex::ShadingMode mode)
{
    if (!compiler) return {};

    const HdMaterialNetwork2 network = ToNetwork2(resource);
    if (network.nodes.empty()) return {};

    const auto terminal = network.terminals.find(HdMaterialTerminalTokens->surface);
    if (terminal == network.terminals.end()) return {};
    const auto node = network.nodes.find(terminal->second.upstreamNode);
    if (node == network.nodes.end()) return {};
    if (IsUsdNativeShaderNetwork(resource)) {
        throw std::runtime_error(
            "the network contains USD-native Usd* shader nodes and is not a "
            "MaterialX shader network; author MaterialX NodeDefs in the mtlx "
            "render context to use their MaterialX implementations");
    }

    const MaterialX::DocumentPtr document = HdMtlxCreateMtlxDocumentFromHdNetwork(
        network,
        node->second,
        terminal->second.upstreamNode,
        materialPath,
        HdMtlxStdLibraries());
    if (!document) return {};

    hdcodex::MaterialXCompiledShader compiled = compiler->CompileDocument(
        document, materialPath.GetName(), mode);
    if (compiled.closure) {
        compiled.closure->id = materialPath.GetString();
        // Provenance is retained for diagnostics only. Closure behavior has
        // already been compiled solely from the expanded MaterialX program.
        compiled.closure->shaderNodeId = node->second.nodeTypeId.GetString();
    }
    return std::make_shared<const hdcodex::MaterialXCompiledShader>(
        std::move(compiled));
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
            if (IsUsdNativeShaderNetwork(resource)) {
                param->GetScene()->RemoveMaterial(GetId().GetString());
                TF_WARN(
                    "hdCodex material %s contains USD-native Usd* shader nodes; "
                    "it is not a MaterialX shader network and is unsupported. "
                    "Author MaterialX NodeDefs in the mtlx render context to "
                    "use their MaterialX implementations.",
                    GetId().GetText());
            } else {
                try {
                    compiled = CompileMaterialX(
                        resource, GetId(), param->GetMaterialCompiler(),
                        param->GetShadingMode());
                    if (!compiled || !compiled->closure) {
                        param->GetScene()->RemoveMaterial(GetId().GetString());
                        TF_WARN(
                            "hdCodex MaterialX material %s has no generated "
                            "path-tracing closure program.",
                            GetId().GetText());
                    } else {
                        hdcodex::SceneMaterial material = *compiled->closure;
                        AttachGeneratedProgram(
                            material, *compiled, param->GetScene());
                        param->GetScene()->UpsertMaterial(std::move(material));
                    }
                } catch (const std::exception& error) {
                    param->GetScene()->RemoveMaterial(GetId().GetString());
                    TF_WARN("hdCodex could not compile MaterialX material %s: %s",
                            GetId().GetText(), error.what());
                }
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
