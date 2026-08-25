#pragma once

#include "hdcodex/core/versioned_scene.h"

#include <optional>
#include <string>

namespace hdcodex {

enum class TextureColorSpace {
    Raw,
    Srgb,
    Auto,
};

/// Decodes a texture through Hio and deduplicates it in the versioned scene.
/// preserveDynamicRange stores linear float pixels for light emission maps.
std::string LoadSceneTexture(
    VersionedScene* scene,
    const std::optional<std::string>& path,
    TextureColorSpace colorSpace,
    bool preserveDynamicRange);

} // namespace hdcodex
