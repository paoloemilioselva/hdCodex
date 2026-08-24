#include "material.h"

#include "render_param.h"

#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/gf/half.h"
#include "pxr/imaging/hio/image.h"
#include "pxr/imaging/hio/types.h"
#include "pxr/usd/sdf/assetPath.h"

#if defined(HDCODEX_HAS_MATERIALX)
#include "pxr/imaging/hdMtlx/hdMtlx.h"
#endif

#include <algorithm>
#include <array>
#include <optional>
#include <cmath>
#include <cstring>
#include <set>
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

float FloatParameter(const HdMaterialNode2& node, const char* name, float fallback)
{
    const auto found = node.parameters.find(TfToken(name));
    if (found == node.parameters.end()) return fallback;
    const VtValue& value = found->second;
    if (value.IsHolding<float>()) return value.UncheckedGet<float>();
    if (value.IsHolding<double>()) return static_cast<float>(value.UncheckedGet<double>());
    if (value.IsHolding<GfVec3f>()) return value.UncheckedGet<GfVec3f>()[0];
    if (value.IsHolding<GfVec3d>()) {
        return static_cast<float>(value.UncheckedGet<GfVec3d>()[0]);
    }
    return fallback;
}

bool BoolParameter(const HdMaterialNode2& node, const char* name, bool fallback)
{
    const auto found = node.parameters.find(TfToken(name));
    if (found == node.parameters.end()) return fallback;
    const VtValue& value = found->second;
    if (value.IsHolding<bool>()) return value.UncheckedGet<bool>();
    if (value.IsHolding<int>()) return value.UncheckedGet<int>() != 0;
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

std::optional<std::string> FileParameter(const HdMaterialNode2& node)
{
    const auto found = node.parameters.find(TfToken("file"));
    if (found == node.parameters.end()) return std::nullopt;
    const VtValue& value = found->second;
    if (value.IsHolding<SdfAssetPath>()) {
        const SdfAssetPath& asset = value.UncheckedGet<SdfAssetPath>();
        return asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
    }
    if (value.IsHolding<std::string>()) return value.UncheckedGet<std::string>();
    if (value.IsHolding<TfToken>()) return value.UncheckedGet<TfToken>().GetString();
    return std::nullopt;
}

std::optional<std::string> FindTextureRecursive(
    const HdMaterialNetwork2& network,
    const SdfPath& nodePath,
    std::set<SdfPath>& visited)
{
    if (!visited.insert(nodePath).second) return std::nullopt;
    const auto found = network.nodes.find(nodePath);
    if (found == network.nodes.end()) return std::nullopt;
    const HdMaterialNode2& node = found->second;
    if (node.nodeTypeId.GetString().find("image") != std::string::npos) {
        if (const auto file = FileParameter(node); file && !file->empty()) return file;
    }
    for (const auto& [inputName, connections] : node.inputConnections) {
        (void)inputName;
        for (const HdMaterialConnection2& connection : connections) {
            if (const auto path = FindTextureRecursive(
                    network, connection.upstreamNode, visited)) return path;
        }
    }
    return std::nullopt;
}

std::optional<std::string> TextureForInput(
    const HdMaterialNetwork2& network,
    const HdMaterialNode2& surface,
    const char* inputName)
{
    const auto found = surface.inputConnections.find(TfToken(inputName));
    if (found == surface.inputConnections.end()) return std::nullopt;
    for (const HdMaterialConnection2& connection : found->second) {
        std::set<SdfPath> visited;
        if (const auto path = FindTextureRecursive(
                network, connection.upstreamNode, visited)) return path;
    }
    return std::nullopt;
}

std::string LoadTexture(
    hdcodex::VersionedScene* scene,
    const std::optional<std::string>& path,
    bool srgb)
{
    if (!scene || !path || path->empty()) return {};
    const std::string id = *path + (srgb ? "#srgb" : "#raw");
    if (scene->HasTexture(id)) return id;

    const HioImageSharedPtr image = HioImage::OpenForReading(
        *path, 0, 0, srgb ? HioImage::SRGB : HioImage::Raw, true);
    if (!image || image->GetWidth() <= 0 || image->GetHeight() <= 0) {
        TF_WARN("hdCodex could not open texture %s", path->c_str());
        return {};
    }

    hdcodex::SceneTexture texture;
    texture.id = id;
    texture.sourcePath = *path;
    texture.width = static_cast<std::uint32_t>(image->GetWidth());
    texture.height = static_cast<std::uint32_t>(image->GetHeight());
    texture.srgb = srgb;
    const HioFormat nativeFormat = image->GetFormat();
    const int channelCount = HioGetComponentCount(nativeFormat);
    const std::size_t componentSize = HioGetDataSizeOfType(nativeFormat);
    if (channelCount <= 0 || componentSize == 0 || HioIsCompressed(nativeFormat)) {
        TF_WARN("hdCodex does not support native texture format %d for %s",
                static_cast<int>(nativeFormat), path->c_str());
        return {};
    }
    std::vector<std::uint8_t> nativePixels(
        static_cast<std::size_t>(texture.width) * texture.height *
        static_cast<std::size_t>(channelCount) * componentSize);
    HioImage::StorageSpec storage;
    storage.width = image->GetWidth();
    storage.height = image->GetHeight();
    storage.depth = 1;
    storage.format = nativeFormat;
    storage.flipped = true;
    storage.data = nativePixels.data();
    if (!image->Read(storage)) {
        TF_WARN("hdCodex could not decode texture %s", path->c_str());
        return {};
    }

    const HioType componentType = HioGetHioType(nativeFormat);
    const auto component = [&](const std::uint8_t* address) {
        switch (componentType) {
        case HioTypeUnsignedByte:
        case HioTypeUnsignedByteSRGB:
            return static_cast<float>(*address) / 255.0F;
        case HioTypeUnsignedShort: {
            std::uint16_t value = 0;
            std::memcpy(&value, address, sizeof(value));
            return static_cast<float>(value) / 65535.0F;
        }
        case HioTypeHalfFloat: {
            GfHalf value;
            std::memcpy(&value, address, sizeof(value));
            return static_cast<float>(value);
        }
        case HioTypeFloat: {
            float value = 0.0F;
            std::memcpy(&value, address, sizeof(value));
            return value;
        }
        default:
            return 0.0F;
        }
    };
    const std::size_t pixelCount =
        static_cast<std::size_t>(texture.width) * texture.height;
    texture.rgba.resize(pixelCount * 4U);
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const std::uint8_t* source = nativePixels.data() + pixel *
            static_cast<std::size_t>(channelCount) * componentSize;
        const auto readChannel = [&](int channel, float fallback) {
            return channel < channelCount
                ? component(source + static_cast<std::size_t>(channel) * componentSize)
                : fallback;
        };
        const float red = readChannel(0, 0.0F);
        const float green = readChannel(1, red);
        const float blue = readChannel(2, red);
        const float alpha = readChannel(3, 1.0F);
        const std::array values = {red, green, blue, alpha};
        for (std::size_t channel = 0; channel < values.size(); ++channel) {
            texture.rgba[pixel * 4U + channel] = static_cast<std::uint8_t>(
                std::lround(std::clamp(values[channel], 0.0F, 1.0F) * 255.0F));
        }
    }
    scene->UpsertTexture(std::move(texture));
    return id;
}

hdcodex::SceneMaterial ExtractSceneMaterial(
    const VtValue& resource,
    const SdfPath& materialPath,
    hdcodex::VersionedScene* scene)
{
    hdcodex::SceneMaterial material;
    material.id = materialPath.GetString();
    const HdMaterialNetwork2 network = ToNetwork2(resource);
    const HdMaterialNode2* surface = nullptr;
    if (const auto terminal = network.terminals.find(HdMaterialTerminalTokens->surface);
        terminal != network.terminals.end()) {
        if (const auto node = network.nodes.find(terminal->second.upstreamNode);
            node != network.nodes.end()) surface = &node->second;
    }
    if (!surface) {
        for (const auto& [path, node] : network.nodes) {
            (void)path;
            const std::string type = node.nodeTypeId.GetString();
            if (type.find("standard_surface") != std::string::npos ||
                type.find("UsdPreviewSurface") != std::string::npos) {
                surface = &node;
                break;
            }
        }
    }
    if (!surface) return material;

    const HdMaterialNode2& node = *surface;
    const std::string type = node.nodeTypeId.GetString();
    if (type.find("standard_surface") != std::string::npos) {
        material.baseColor = ColorParameter(node, "base_color", material.baseColor);
        material.metalness = FloatParameter(node, "metalness", material.metalness);
        material.roughness = FloatParameter(node, "specular_roughness", material.roughness);
        material.emissionWeight = FloatParameter(node, "emission", 0.0F);
        material.emission = ColorParameter(node, "emission_color", material.emission);
        for (float& component : material.emission) component *= material.emissionWeight;
        material.opacity = FloatParameter(node, "opacity", material.opacity);
        material.transmission = FloatParameter(node, "transmission", material.transmission);
        material.transmissionColor = ColorParameter(
            node, "transmission_color", material.transmissionColor);
        material.indexOfRefraction = FloatParameter(
            node, "specular_IOR", material.indexOfRefraction);
        material.thinWalled = BoolParameter(node, "thin_walled", material.thinWalled);
        material.subsurface = FloatParameter(node, "subsurface", material.subsurface);
        material.subsurfaceColor = ColorParameter(
            node, "subsurface_color", material.subsurfaceColor);
        material.subsurfaceRadius = ColorParameter(
            node, "subsurface_radius", material.subsurfaceRadius);
        material.subsurfaceScale = FloatParameter(
            node, "subsurface_scale", material.subsurfaceScale);
        material.baseColorTexture = LoadTexture(
            scene, TextureForInput(network, node, "base_color"), true);
        material.metalnessTexture = LoadTexture(
            scene, TextureForInput(network, node, "metalness"), false);
        material.roughnessTexture = LoadTexture(
            scene, TextureForInput(network, node, "specular_roughness"), false);
        material.emissionTexture = LoadTexture(
            scene, TextureForInput(network, node, "emission_color"), true);
        material.opacityTexture = LoadTexture(
            scene, TextureForInput(network, node, "opacity"), false);
        material.normalTexture = LoadTexture(
            scene, TextureForInput(network, node, "normal"), false);
        material.transmissionTexture = LoadTexture(
            scene, TextureForInput(network, node, "transmission_color"), true);
        material.subsurfaceTexture = LoadTexture(
            scene, TextureForInput(network, node, "subsurface"), false);
        material.subsurfaceColorTexture = LoadTexture(
            scene, TextureForInput(network, node, "subsurface_color"), true);
        material.subsurfaceRadiusTexture = LoadTexture(
            scene, TextureForInput(network, node, "subsurface_radius"), false);
        return material;
    }
    if (type.find("UsdPreviewSurface") != std::string::npos) {
        material.baseColor = ColorParameter(node, "diffuseColor", material.baseColor);
        material.metalness = FloatParameter(node, "metallic", material.metalness);
        material.roughness = FloatParameter(node, "roughness", material.roughness);
        material.emission = ColorParameter(node, "emissiveColor", material.emission);
        material.emissionWeight = 1.0F;
        material.opacity = FloatParameter(node, "opacity", material.opacity);
        material.indexOfRefraction = FloatParameter(node, "ior", material.indexOfRefraction);
        material.baseColorTexture = LoadTexture(
            scene, TextureForInput(network, node, "diffuseColor"), true);
        material.metalnessTexture = LoadTexture(
            scene, TextureForInput(network, node, "metallic"), false);
        material.roughnessTexture = LoadTexture(
            scene, TextureForInput(network, node, "roughness"), false);
        material.emissionTexture = LoadTexture(
            scene, TextureForInput(network, node, "emissiveColor"), true);
        material.opacityTexture = LoadTexture(
            scene, TextureForInput(network, node, "opacity"), false);
        material.normalTexture = LoadTexture(
            scene, TextureForInput(network, node, "normal"), false);
        return material;
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
            param->GetScene()->UpsertMaterial(
                ExtractSceneMaterial(resource, GetId(), param->GetScene()));
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
