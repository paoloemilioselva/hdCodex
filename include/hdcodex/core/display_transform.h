#pragma once

#include <array>

namespace hdcodex {

// Converts scene-linear Rec.709/sRGB primaries to display-encoded sRGB.
// The neutral highlight compression is applied before the sRGB transfer
// function so HDR render outputs remain unclipped without changing the
// renderer's linear AOV contract.
std::array<float, 3> SceneLinearToDisplaySrgb(
    const std::array<float, 3>& color,
    float exposureStops = 0.0F) noexcept;

} // namespace hdcodex
