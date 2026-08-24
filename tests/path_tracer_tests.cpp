#include "hdcodex/core/shader_cache.h"
#include "hdcodex/core/versioned_scene.h"
#include "hdcodex/gpu/vulkan_context.h"
#include "hdcodex/gpu/vulkan_path_tracer.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
try {
    const auto cacheRoot = std::filesystem::temp_directory_path() / "hdcodex-path-tracer-test";
    hdcodex::ShaderCache cache(cacheRoot);
    hdcodex::VulkanContext context;
    hdcodex::VulkanPathTracer tracer(context, cache);

    auto scene = std::make_shared<hdcodex::SceneSnapshot>();
    scene->revision = 1;
    scene->meshes.push_back({
        .id = "/triangle",
        .positions = {-1.25F, -1.0F, -3.0F, 1.25F, -1.0F, -3.0F, 0.0F, 1.2F, -3.0F},
        .indices = {0, 1, 2},
        .texcoords = {0.0F, 0.0F, 1.0F, 0.0F, 0.5F, 1.0F},
        .materialId = "/red",
    });
    scene->materials.push_back({
        .id = "/red",
        .baseColor = {1.0F, 1.0F, 1.0F},
        .metalness = 0.15F,
        .roughness = 0.3F,
        .baseColorTexture = "green#srgb",
    });
    scene->textures.push_back({
        .id = "green#srgb",
        .sourcePath = "synthetic",
        .width = 1,
        .height = 1,
        .srgb = true,
        .rgba = {16, 220, 32, 255},
    });
    tracer.SetScene(scene);
    Check(tracer.HasGeometry(), "path tracer did not build geometry");

    const hdcodex::PathTracerCamera camera{
        .origin = {0.0F, 0.0F, 0.0F},
        .lowerLeft = {-1.0F, -1.0F, -1.0F},
        .horizontal = {2.0F, 0.0F, 0.0F},
        .vertical = {0.0F, 2.0F, 0.0F},
    };
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    const auto pixels = tracer.Render(camera, width, height, 0);
    Check(pixels.size() == width * height * 4U, "path tracer returned wrong image size");
    for (float value : pixels) Check(std::isfinite(value), "path tracer returned non-finite output");

    const std::size_t center = (height / 2U * width + width / 2U) * 4U;
    const std::size_t corner = 0;
    const float centerLuma = pixels[center] + pixels[center + 1] + pixels[center + 2];
    const float cornerLuma = pixels[corner] + pixels[corner + 1] + pixels[corner + 2];
    Check(std::abs(centerLuma - cornerLuma) > 0.02F,
          "ray-query image does not distinguish triangle from background");
    Check(pixels[center + 1] > pixels[center] * 1.5F,
          "path tracer did not sample the bound base-color texture");
    Check(pixels[center + 3] == 1.0F, "path tracer alpha is not one");

    const auto progressive = tracer.Render(camera, width, height, 1);
    Check(progressive.size() == pixels.size(), "progressive image size changed");
    std::error_code error;
    std::filesystem::remove_all(cacheRoot, error);
    std::cout << "Vulkan BLAS/TLAS ray-query path trace passed on "
              << context.DeviceInfo().name << '\n';
    return 0;
} catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
}
