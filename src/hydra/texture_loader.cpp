#include "texture_loader.h"

#include "pxr/base/gf/half.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hio/image.h"
#include "pxr/imaging/hio/types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace hdcodex {
namespace {

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
    if (scene->HasTexture(id)) return id;

    const HioImageSharedPtr image = HioImage::OpenForReading(
        *path, 0, 0, ToHioColorSpace(colorSpace), true);
    if (!image || image->GetWidth() <= 0 || image->GetHeight() <= 0) {
        TF_WARN("hdCodex could not open texture %s", path->c_str());
        return {};
    }

    SceneTexture texture;
    texture.id = id;
    texture.sourcePath = *path;
    texture.width = static_cast<std::uint32_t>(image->GetWidth());
    texture.height = static_cast<std::uint32_t>(image->GetHeight());
    texture.srgb = !preserveDynamicRange && colorSpace == TextureColorSpace::Srgb;
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
    if (preserveDynamicRange) texture.rgbaFloat.resize(pixelCount * 4U);
    else texture.rgba.resize(pixelCount * 4U);
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const std::uint8_t* source = nativePixels.data() + pixel *
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
    scene->UpsertTexture(std::move(texture));
    return id;
}

} // namespace hdcodex
