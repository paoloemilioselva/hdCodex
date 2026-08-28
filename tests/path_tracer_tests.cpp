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
    Check(tracer.GetShadingMode() == hdcodex::ShadingMode::Fused,
          "fused shading mode is not the path-tracer default");
    tracer.SetShadingMode(hdcodex::ShadingMode::Modular);
    Check(tracer.GetShadingMode() == hdcodex::ShadingMode::Modular,
          "path tracer did not select modular shading mode");
    tracer.SetShadingMode(hdcodex::ShadingMode::Fused);

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
        .id = "synthetic-udim#srgb#udim=1001",
        .sourcePath = "synthetic.1001",
        .udimSetId = "synthetic-udim#srgb",
        .udimTile = 1001,
        .width = 1,
        .height = 1,
        .srgb = true,
        .rgba = {230, 16, 16, 255},
    });
    scene->textures.push_back({
        .id = "synthetic-udim#srgb#udim=1002",
        .sourcePath = "synthetic.1002",
        .udimSetId = "synthetic-udim#srgb",
        .udimTile = 1002,
        .width = 1,
        .height = 1,
        .srgb = true,
        .rgba = {16, 24, 230, 255},
    });
    scene->textures.push_back({
        .id = "rect#hdr",
        .sourcePath = "synthetic",
        .width = 1,
        .height = 1,
        .srgb = false,
        .rgbaFloat = {2.0F, 2.0F, 2.0F, 1.0F},
    });
    scene->textures.push_back({
        .id = "spectral-gradient#hdr",
        .sourcePath = "synthetic",
        .width = 4,
        .height = 1,
        .srgb = false,
        .rgbaFloat = {
            3.0F, 0.02F, 0.02F, 1.0F,
            0.02F, 3.0F, 0.02F, 1.0F,
            0.02F, 0.02F, 3.0F, 1.0F,
            3.0F, 3.0F, 3.0F, 1.0F,
        },
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
        return tracer.Render(camera, width, height, 0U, 8U, sampleCount);
    };
    const auto sequentialFirst = tracer.Render(camera, width, height, 0U);
    const auto sequentialTwo = tracer.Render(camera, width, height, 1U);
    const auto batchedTwo = tracer.Render(camera, width, height, 0U, 8U, 2U);
    Check(batchedTwo.size() == sequentialTwo.size(), "batched image size changed");
    for (std::size_t index = 0; index < batchedTwo.size(); ++index) {
        Check(std::abs(batchedTwo[index] - sequentialTwo[index]) < 1e-5F,
              "batched samples changed the progressive estimator");
    }
    Check(sequentialFirst.size() == batchedTwo.size(),
          "single-sample image size changed");

    const auto pixels = renderSamples(64U);
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

    scene->materials.front().diffuseModel =
        hdcodex::SceneMaterial::DiffuseModel::OrenNayar;
    scene->materials.front().diffuseRoughness = 0.85F;
    tracer.SetScene(scene);
    const auto roughDiffuse = renderSamples(64U);
    float roughDiffuseDifference = 0.0F;
    for (std::size_t index = 0; index < roughDiffuse.size(); index += 4U) {
        roughDiffuseDifference +=
            std::abs(roughDiffuse[index] - pixels[index]) +
            std::abs(roughDiffuse[index + 1U] - pixels[index + 1U]) +
            std::abs(roughDiffuse[index + 2U] - pixels[index + 2U]);
    }
    Check(roughDiffuseDifference > 0.05F,
          "MaterialX Oren-Nayar roughness did not affect GPU shading");
    scene->materials.front().diffuseModel =
        hdcodex::SceneMaterial::DiffuseModel::Lambert;
    scene->materials.front().diffuseRoughness = 0.0F;
    tracer.SetScene(scene);

    scene->meshes.front().texcoords = {
        1.1F, 0.1F, 1.9F, 0.1F, 1.5F, 0.9F,
    };
    scene->materials.front().baseColorTexture = "synthetic-udim#srgb";
    tracer.SetScene(scene);
    const auto udimPixels = renderSamples(64U);
    Check(udimPixels[center + 2] > udimPixels[center] * 2.0F,
          "path tracer did not select the authored UDIM tile");
    scene->meshes.front().texcoords = {
        0.0F, 0.0F, 1.0F, 0.0F, 0.5F, 1.0F,
    };
    scene->materials.front().baseColorTexture = "green#srgb";
    tracer.SetScene(scene);

    const auto progressive = tracer.Render(camera, width, height, 64U);
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
    const auto authoredNormals = renderSamples(64U);
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
    const float glossyLuma = glossy[center] + glossy[center + 1] + glossy[center + 2];
    const float matteBlackLuma = matteBlack[center] + matteBlack[center + 1] +
        matteBlack[center + 2];
    Check(glossyLuma > matteBlackLuma + 0.1F,
          "dielectric GGX specular highlight was not evaluated");

    scene->materials.front().coat = 1.0F;
    scene->materials.front().coatRoughness = 0.08F;
    tracer.SetScene(scene);
    const auto coated = renderSamples(64U);
    const float coatedLuma = coated[center] + coated[center + 1] + coated[center + 2];
    Check(coatedLuma > matteBlackLuma + 0.1F,
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
    const auto scattered = renderSamples(64U);
    Check(scattered[center + 2] > scattered[center + 1] * 1.5F,
          "subsurface parameters did not reach the GPU material ABI");

    scene->materials.front().subsurface = 0.0F;
    scene->materials.front().subsurfaceTexture.clear();

    scene->materials.front().opacity = 0.0F;
    tracer.SetScene(scene);
    const auto cutout = renderSamples(64U);
    Check(cutout[center + 2] > cutout[center + 1],
          "zero-opacity surface did not reveal the blue environment");

    scene->materials.front().opacity = 1.0F;
    scene->materials.front().transmission = 1.0F;
    scene->materials.front().transmissionColor = {1.0F, 0.05F, 0.02F};
    scene->materials.front().thinWalled = true;
    scene->materials.front().metalness = 0.0F;
    scene->materials.front().specularWeight = 0.0F;
    tracer.SetScene(scene);
    const auto transmitted = renderSamples(64U);
    Check(transmitted[center] > transmitted[center + 1] * 2.0F,
          "thin-walled transmission did not tint the environment");

    scene->materials.front().thinWalled = false;
    scene->materials.front().transmissionColor = {1.0F, 1.0F, 1.0F};
    scene->materials.front().transmissionDispersionScale = 0.0F;
    scene->lights.resize(1U);
    scene->lights.front().color = {1.0F, 1.0F, 1.0F};
    scene->lights.front().intensity = 1.0F;
    scene->lights.front().texture = "spectral-gradient#hdr";
    tracer.SetScene(scene);
    const auto nondispersive = renderSamples(64U);
    scene->materials.front().transmissionDispersionScale = 1.0F;
    scene->materials.front().transmissionDispersionAbbeNumber = 9.0F;
    tracer.SetScene(scene);
    const auto dispersive = renderSamples(64U);
    float dispersionDifference = 0.0F;
    for (std::size_t index = 0; index < dispersive.size(); index += 4U) {
        dispersionDifference +=
            std::abs(dispersive[index] - nondispersive[index]) +
            std::abs(dispersive[index + 1U] - nondispersive[index + 1U]) +
            std::abs(dispersive[index + 2U] - nondispersive[index + 2U]);
    }
    Check(dispersionDifference > 0.1F,
          "wavelength-dependent dielectric IOR did not affect refraction");

    scene->lights.clear();
    scene->meshes.front().materialId.clear();
    scene->meshes.front().displayColor = {0.05F, 0.8F, 0.1F};
    scene->materials.front().baseColor = {0.8F, 0.8F, 0.8F};
    scene->materials.front().baseColorTexture.clear();
    scene->materials.front().roughness = 1.0F;
    scene->materials.front().transmission = 0.0F;
    scene->materials.front().thinWalled = false;
    tracer.SetScene(scene);
    const auto fallbackLighting = renderSamples(64U);
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
    const auto zUpFallback = zUpTracer.Render(
        zUpCamera, width, height, 0U, 8U, 64U);
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
