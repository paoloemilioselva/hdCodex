#include "hdcodex/core/shader_cache.h"
#include "hdcodex/core/versioned_scene.h"
#include "hdcodex/gpu/vulkan_context.h"
#include "hdcodex/gpu/vulkan_path_tracer.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

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
    scene->textures.push_back({
        .id = "scatter#raw",
        .sourcePath = "synthetic",
        .width = 1,
        .height = 1,
        .srgb = false,
        .rgba = {255, 255, 255, 255},
    });
    scene->textures.push_back({
        .id = "rect#hdr",
        .sourcePath = "synthetic",
        .width = 1,
        .height = 1,
        .srgb = false,
        .rgbaFloat = {2.0F, 2.0F, 2.0F, 1.0F},
    });
    scene->lights.push_back({
        .id = "/dome",
        .type = hdcodex::SceneLightType::Dome,
        .color = {0.08F, 0.16F, 0.8F},
        .intensity = 0.35F,
    });
    scene->lights.push_back({
        .id = "/rect",
        .type = hdcodex::SceneLightType::Rect,
        .color = {1.0F, 0.92F, 0.82F},
        .intensity = 50.0F,
        .texture = "rect#hdr",
        .basisX = {0.817F, -0.576F, 0.0F},
        .basisY = {0.184F, 0.261F, -0.947F},
        .basisZ = {0.547F, 0.775F, 0.319F},
        .position = {3.282F, 4.650F, -1.086F},
        .axisU = {1.634F, -1.152F, 0.0F},
        .axisV = {0.368F, 0.522F, -1.894F},
        .width = 2.0F,
        .height = 2.0F,
        .area = 4.0F,
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
    const auto renderSamples = [&](std::uint32_t sampleCount) {
        std::vector<float> result;
        for (std::uint32_t sample = 0; sample < sampleCount; ++sample) {
            result = tracer.Render(camera, width, height, sample);
        }
        return result;
    };
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

    const auto interactive = tracer.Render(camera, width / 2U, height / 2U, 0U, 2U);
    Check(interactive.size() == width * height,
          "reduced-resolution interactive render size is incorrect");

    scene->meshes.front().normals = {
        0.6F, 0.85F, 0.35F,
        0.6F, 0.85F, 0.35F,
        0.6F, 0.85F, 0.35F,
    };
    tracer.SetScene(scene);
    const auto authoredNormals = tracer.Render(camera, width, height, 0);
    Check(std::abs(authoredNormals[center + 1] - pixels[center + 1]) > 0.02F,
          "authored mesh normals did not affect GPU shading");
    scene->meshes.front().normals.clear();

    scene->meshes.front().normals = {
        0.3446F, 0.4881F, 0.8027F,
        0.3446F, 0.4881F, 0.8027F,
        0.3446F, 0.4881F, 0.8027F,
    };
    scene->materials.front().baseColor = {0.0F, 0.0F, 0.0F};
    scene->materials.front().baseColorTexture.clear();
    scene->materials.front().roughness = 0.08F;
    scene->materials.front().specularWeight = 1.0F;
    tracer.SetScene(scene);
    const auto glossy = renderSamples(64U);
    scene->materials.front().specularWeight = 0.0F;
    tracer.SetScene(scene);
    const auto matteBlack = renderSamples(64U);
    Check(glossy[center] > matteBlack[center] + 0.1F,
          "dielectric GGX specular highlight was not evaluated");

    scene->materials.front().coat = 1.0F;
    scene->materials.front().coatRoughness = 0.08F;
    tracer.SetScene(scene);
    const auto coated = renderSamples(64U);
    Check(coated[center] > matteBlack[center] + 0.1F,
          "Standard Surface coat highlight was not evaluated");

    scene->meshes.front().normals.clear();
    scene->materials.front().baseColor = {1.0F, 1.0F, 1.0F};
    scene->materials.front().baseColorTexture = "green#srgb";
    scene->materials.front().roughness = 0.3F;
    scene->materials.front().specularWeight = 1.0F;
    scene->materials.front().coat = 0.0F;

    scene->materials.front().subsurfaceColor = {0.02F, 0.05F, 1.0F};
    scene->materials.front().subsurfaceScale = 0.0F;
    scene->materials.front().subsurfaceTexture = "scatter#raw";
    tracer.SetScene(scene);
    const auto scattered = tracer.Render(camera, width, height, 0);
    Check(scattered[center + 2] > scattered[center + 1] * 1.5F,
          "subsurface parameters did not reach the GPU material ABI");

    scene->materials.front().subsurface = 0.0F;
    scene->materials.front().subsurfaceTexture.clear();

    scene->materials.front().opacity = 0.0F;
    tracer.SetScene(scene);
    const auto cutout = tracer.Render(camera, width, height, 0);
    Check(cutout[center + 2] > cutout[center + 1],
          "zero-opacity surface did not reveal the blue environment");

    scene->materials.front().opacity = 1.0F;
    scene->materials.front().transmission = 1.0F;
    scene->materials.front().transmissionColor = {1.0F, 0.05F, 0.02F};
    scene->materials.front().thinWalled = true;
    scene->materials.front().metalness = 0.0F;
    scene->materials.front().specularWeight = 0.0F;
    tracer.SetScene(scene);
    const auto transmitted = tracer.Render(camera, width, height, 0);
    Check(transmitted[center] > transmitted[center + 1] * 2.0F,
          "thin-walled transmission did not tint the environment");

    scene->lights.clear();
    scene->meshes.front().materialId.clear();
    scene->meshes.front().displayColor = {0.05F, 0.8F, 0.1F};
    scene->materials.front().baseColor = {0.8F, 0.8F, 0.8F};
    scene->materials.front().baseColorTexture.clear();
    scene->materials.front().roughness = 1.0F;
    scene->materials.front().transmission = 0.0F;
    scene->materials.front().thinWalled = false;
    tracer.SetScene(scene);
    const auto fallbackLighting = tracer.Render(camera, width, height, 0);
    const float fallbackCenterLuma = fallbackLighting[center] +
        fallbackLighting[center + 1] + fallbackLighting[center + 2];
    const float fallbackCornerLuma = fallbackLighting[corner] +
        fallbackLighting[corner + 1] + fallbackLighting[corner + 2];
    Check(fallbackCenterLuma > 0.05F,
          "default sunlight did not illuminate a lightless scene");
    Check(fallbackLighting[center + 1] > fallbackLighting[center] * 2.0F,
          "unbound mesh did not use its Hydra display color");
    Check(fallbackCornerLuma > 0.05F,
          "default sky did not illuminate a lightless scene background");

    scene->meshes.front().positions = {
        -1.25F, -3.0F, -1.0F,
         0.0F,  -3.0F,  1.2F,
         1.25F, -3.0F, -1.0F,
    };
    hdcodex::VulkanPathTracer zUpTracer(context, cache);
    zUpTracer.SetScene(scene);
    const hdcodex::PathTracerCamera zUpCamera{
        .origin = {0.0F, 0.0F, 0.0F},
        .lowerLeft = {-1.0F, -1.0F, -1.0F},
        .horizontal = {2.0F, 0.0F, 0.0F},
        .vertical = {0.0F, 0.0F, 2.0F},
    };
    const auto zUpFallback = zUpTracer.Render(zUpCamera, width, height, 0);
    const float zUpCenterLuma = zUpFallback[center] +
        zUpFallback[center + 1] + zUpFallback[center + 2];
    Check(zUpCenterLuma > 0.05F,
          "default sunlight did not illuminate a Z-up lightless scene");
    std::error_code error;
    std::filesystem::remove_all(cacheRoot, error);
    std::cout << "Vulkan BLAS/TLAS ray-query path trace passed on "
              << context.DeviceInfo().name << '\n';
    return 0;
} catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
}
