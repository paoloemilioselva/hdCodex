#pragma once

#include <optional>
#include <string_view>

namespace hdcodex {

/// Selects the physical organization of generated MaterialX shading code.
/// Material semantics must not vary between these modes.
enum class ShadingMode {
    Fused,
    Modular,
    RasterPreview,
};

[[nodiscard]] constexpr std::string_view ShadingModeName(ShadingMode mode) noexcept
{
    switch (mode) {
    case ShadingMode::Fused: return "fused";
    case ShadingMode::Modular: return "modular";
    case ShadingMode::RasterPreview: return "raster";
    }
    return "fused";
}

[[nodiscard]] constexpr std::optional<ShadingMode> ParseShadingMode(
    std::string_view name) noexcept
{
    if (name == "fused" || name == "fused-specialized") {
        return ShadingMode::Fused;
    }
    if (name == "modular" || name == "instrumented") {
        return ShadingMode::Modular;
    }
    if (name == "raster" || name == "raster-preview") {
        return ShadingMode::RasterPreview;
    }
    return std::nullopt;
}

} // namespace hdcodex
