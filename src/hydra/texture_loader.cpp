#include "texture_loader.h"

#include "pxr/base/gf/half.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hio/image.h"
#include "pxr/imaging/hio/types.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/usdShade/udimUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace hdcodex {
namespace {

// Only UDIM tiles are residency-limited. Ordinary material images must retain
// their authored dimensions: the StandardShaderBall detail maps are larger
// than the gallery output and visibly lose labels and surface detail if they
// are reduced to the output resolution.
constexpr int kMaxUdimTextureDimension = 1024;

HioImage::SourceColorSpace ToHioColorSpace(TextureColorSpace colorSpace)
{
    if (colorSpace == TextureColorSpace::Raw) return HioImage::Raw;
    if (colorSpace == TextureColorSpace::Srgb) return HioImage::SRGB;
    return HioImage::Auto;
}

float SrgbToLinear(float value)
{
    return value <= 0.04045F
        ? value / 12.92F
        : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

std::string ResolveTexturePath(const std::string& sourcePath)
{
    const ArResolvedPath direct = ArGetResolver().Resolve(sourcePath);
    if (!direct.empty()) return direct.GetPathString();

    // HdMaterialNetwork2 retains the authored SdfAssetPath but not its
    // property stack. Recover the authoring anchor from the live layer set.
    for (const SdfLayerHandle& layer : SdfLayer::GetLoadedLayers()) {
        if (!layer || layer->IsAnonymous()) continue;
        const std::string anchored = layer->ComputeAbsolutePath(sourcePath);
        const ArResolvedPath resolved = ArGetResolver().Resolve(anchored);
        if (!resolved.empty()) return resolved.GetPathString();
    }
    return sourcePath;
}

bool DecodeTexture(
    VersionedScene* scene,
    const std::string& sourcePath,
    const std::string& id,
    TextureColorSpace colorSpace,
    bool preserveDynamicRange,
    const std::string& udimSetId = {},
    std::uint32_t udimTile = 0U)
{
    if (scene->HasTexture(id)) return true;

    const HioImageSharedPtr image = HioImage::OpenForReading(
        sourcePath, 0, 0, ToHioColorSpace(colorSpace), true);
    if (!image || image->GetWidth() <= 0 || image->GetHeight() <= 0) {
        TF_WARN("hdCodex could not open texture %s", sourcePath.c_str());
        return false;
    }

    SceneTexture texture;
    texture.id = id;
    texture.sourcePath = sourcePath;
    texture.udimSetId = udimSetId;
    texture.udimTile = udimTile;
    int targetWidth = image->GetWidth();
    int targetHeight = image->GetHeight();
    if (!preserveDynamicRange && !udimSetId.empty() &&
        std::max(targetWidth, targetHeight) > kMaxUdimTextureDimension) {
        const float scale = static_cast<float>(kMaxUdimTextureDimension) /
            static_cast<float>(std::max(targetWidth, targetHeight));
        targetWidth = std::max(1, static_cast<int>(std::lround(targetWidth * scale)));
        targetHeight = std::max(1, static_cast<int>(std::lround(targetHeight * scale)));
    }
    texture.width = static_cast<std::uint32_t>(targetWidth);
    texture.height = static_cast<std::uint32_t>(targetHeight);
    texture.srgb = !preserveDynamicRange && colorSpace == TextureColorSpace::Srgb;
    const HioFormat nativeFormat = image->GetFormat();
    const int channelCount = HioGetComponentCount(nativeFormat);
    const std::size_t componentSize = HioGetDataSizeOfType(nativeFormat);
    if (channelCount <= 0 || componentSize == 0 || HioIsCompressed(nativeFormat)) {
        TF_WARN("hdCodex does not support native texture format %d for %s",
                static_cast<int>(nativeFormat), sourcePath.c_str());
        return false;
    }

    const int sourceWidth = image->GetWidth();
    const int sourceHeight = image->GetHeight();
    std::vector<std::uint8_t> nativePixels(
        static_cast<std::size_t>(sourceWidth) * sourceHeight *
        static_cast<std::size_t>(channelCount) * componentSize);
    HioImage::StorageSpec storage;
    storage.width = sourceWidth;
    storage.height = sourceHeight;
    storage.depth = 1;
    storage.format = nativeFormat;
    storage.flipped = true;
    storage.data = nativePixels.data();
    if (!image->Read(storage)) {
        TF_WARN("hdCodex could not decode texture %s", sourcePath.c_str());
        return false;
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
    if (preserveDynamicRange) texture.rgbaFloat.resize(pixelCount * 4U);
    else texture.rgba.resize(pixelCount * 4U);
    for (std::uint32_t y = 0; y < texture.height; ++y) {
        const std::size_t sourceY = std::min<std::size_t>(
            static_cast<std::size_t>(y) * sourceHeight / texture.height,
            static_cast<std::size_t>(sourceHeight - 1));
        for (std::uint32_t x = 0; x < texture.width; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * texture.width + x;
            const std::size_t sourceX = std::min<std::size_t>(
                static_cast<std::size_t>(x) * sourceWidth / texture.width,
                static_cast<std::size_t>(sourceWidth - 1));
            const std::size_t sourcePixel = sourceY * sourceWidth + sourceX;
            const std::uint8_t* source = nativePixels.data() + sourcePixel *
                static_cast<std::size_t>(channelCount) * componentSize;
            const auto readChannel = [&](int channel, float fallback) {
                return channel < channelCount
                    ? component(source + static_cast<std::size_t>(channel) * componentSize)
                    : fallback;
            };
            float red = readChannel(0, 0.0F);
            float green = readChannel(1, red);
            float blue = readChannel(2, red);
            const float alpha = readChannel(3, 1.0F);
            if (preserveDynamicRange && componentType == HioTypeUnsignedByteSRGB) {
                red = SrgbToLinear(red);
                green = SrgbToLinear(green);
                blue = SrgbToLinear(blue);
            }
            const std::array values = {red, green, blue, alpha};
            if (preserveDynamicRange) {
                std::copy(values.begin(), values.end(),
                          texture.rgbaFloat.begin() + static_cast<std::ptrdiff_t>(pixel * 4U));
            } else {
                for (std::size_t channel = 0; channel < values.size(); ++channel) {
                    texture.rgba[pixel * 4U + channel] = static_cast<std::uint8_t>(
                        std::lround(std::clamp(values[channel], 0.0F, 1.0F) * 255.0F));
                }
            }
        }
    }
    scene->UpsertTexture(std::move(texture));
    return true;
}

} // namespace

std::string LoadSceneTexture(
    VersionedScene* scene,
    const std::optional<std::string>& path,
    TextureColorSpace colorSpace,
    bool preserveDynamicRange)
{
    if (!scene || !path || path->empty()) return {};
    const char* suffix = colorSpace == TextureColorSpace::Srgb
        ? "#srgb" : (colorSpace == TextureColorSpace::Raw ? "#raw" : "#auto");
    const std::string id = *path + suffix + (preserveDynamicRange ? "-hdr" : "");
    constexpr std::string_view udimToken = "<UDIM>";
    const std::size_t udimPosition = path->find(udimToken);
    if (udimPosition == std::string::npos) {
        return DecodeTexture(
            scene, ResolveTexturePath(*path), id, colorSpace, preserveDynamicRange)
            ? id : std::string{};
    }

    bool loadedAnyTile = false;
    // The packed path-tracer handle reserves 23 bits for tiles 1001-1023.
    // This covers the conventional first two UDIM rows and the target scene;
    // wider sets fail explicitly instead of silently sampling the wrong tile.
    std::vector<UsdShadeUdimUtils::ResolvedPathAndTile> resolvedTiles =
        UsdShadeUdimUtils::ResolveUdimTilePaths(*path, SdfLayerHandle());
    if (resolvedTiles.empty()) {
        // A Hydra material resource does not retain the property stack that
        // authored an SdfAssetPath. Search the live layer registry to recover
        // the same anchor that UsdImaging uses while reading the attribute.
        for (const SdfLayerHandle& layer : SdfLayer::GetLoadedLayers()) {
            if (!layer || layer->IsAnonymous()) continue;
            resolvedTiles = UsdShadeUdimUtils::ResolveUdimTilePaths(*path, layer);
            if (!resolvedTiles.empty()) break;
        }
    }
    bool skippedUnsupportedTile = false;
    for (const auto& [resolvedPath, tileText] : resolvedTiles) {
        std::uint32_t tile = 0U;
        try {
            tile = static_cast<std::uint32_t>(std::stoul(tileText));
        } catch (const std::exception&) {
            continue;
        }
        if (tile < 1001U || tile > 1023U) {
            skippedUnsupportedTile = true;
            continue;
        }
        const std::string tileId = id + "#udim=" + std::to_string(tile);
        loadedAnyTile |= DecodeTexture(
            scene, resolvedPath, tileId, colorSpace,
            preserveDynamicRange, id, tile);
    }
    if (skippedUnsupportedTile) {
        TF_WARN("hdCodex UDIM set %s contains tiles beyond the supported 1001-1023 range",
                path->c_str());
    }
    if (!loadedAnyTile) {
        TF_WARN("hdCodex could not resolve any UDIM tiles for %s", path->c_str());
        return {};
    }
    return id;
}

} // namespace hdcodex
