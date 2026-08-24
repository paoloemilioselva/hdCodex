#pragma once

#include "hdcodex/core/versioned_scene.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace hdcodex {

class ShaderCache;
class VulkanContext;

struct PathTracerCamera {
    std::array<float, 3> origin{};
    std::array<float, 3> lowerLeft{};
    std::array<float, 3> horizontal{};
    std::array<float, 3> vertical{};

    bool operator==(const PathTracerCamera&) const = default;
};

/// Headless Vulkan compute path tracer using VK_KHR_ray_query traversal.
class VulkanPathTracer final {
public:
    VulkanPathTracer(VulkanContext& context, ShaderCache& shaderCache);
    ~VulkanPathTracer();

    VulkanPathTracer(const VulkanPathTracer&) = delete;
    VulkanPathTracer& operator=(const VulkanPathTracer&) = delete;

    void SetScene(const std::shared_ptr<const SceneSnapshot>& scene);

    /// Traces one progressive sample per pixel and returns linear RGBA32F.
    [[nodiscard]] std::vector<float> Render(
        const PathTracerCamera& camera,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t sampleIndex);

    [[nodiscard]] bool HasGeometry() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace hdcodex
