#include "hdcodex/core/display_transform.h"

#include <algorithm>
#include <cmath>

namespace hdcodex {
namespace {

float Sanitize(float value) noexcept
{
    return std::isfinite(value) ? std::max(value, 0.0F) : 0.0F;
}

std::array<float, 3> NeutralToneMap(std::array<float, 3> color) noexcept
{
    constexpr float kStartCompression = 0.76F;
    constexpr float kDesaturation = 0.15F;

    const float darkest = std::min({color[0], color[1], color[2]});
    const float offset = darkest < 0.08F
        ? darkest - 6.25F * darkest * darkest
        : 0.04F;
    for (float& channel : color) channel -= offset;

    const float peak = std::max({color[0], color[1], color[2]});
    if (peak < kStartCompression) return color;

    constexpr float distanceToWhite = 1.0F - kStartCompression;
    const float compressedPeak = 1.0F - distanceToWhite * distanceToWhite /
        (peak + distanceToWhite - kStartCompression);
    const float scale = compressedPeak / peak;
    const float desaturation = 1.0F - 1.0F /
        (kDesaturation * (peak - compressedPeak) + 1.0F);
    for (float& channel : color) {
        channel = std::lerp(channel * scale, compressedPeak, desaturation);
    }
    return color;
}

float LinearToSrgb(float value) noexcept
{
    value = std::clamp(value, 0.0F, 1.0F);
    return value <= 0.0031308F
        ? value * 12.92F
        : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

} // namespace

std::array<float, 3> SceneLinearToDisplaySrgb(
    const std::array<float, 3>& color,
    float exposureStops) noexcept
{
    exposureStops = std::isfinite(exposureStops)
        ? std::clamp(exposureStops, -20.0F, 20.0F)
        : 0.0F;
    const float exposure = std::exp2(exposureStops);
    std::array<float, 3> mapped{
        Sanitize(color[0]) * exposure,
        Sanitize(color[1]) * exposure,
        Sanitize(color[2]) * exposure};
    mapped = NeutralToneMap(mapped);
    for (float& channel : mapped) channel = LinearToSrgb(channel);
    return mapped;
}

} // namespace hdcodex
