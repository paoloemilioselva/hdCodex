#include "hdcodex/core/hash.h"
#include "hdcodex/core/shading_mode.h"
#include "hdcodex/core/shader_cache.h"
#include "hdcodex/core/versioned_scene.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestSha256()
{
    Require(hdcodex::Sha256("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "empty SHA-256 vector failed");
    Require(hdcodex::Sha256("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "abc SHA-256 vector failed");
}

void TestShadingModes()
{
    using hdcodex::ShadingMode;
    Require(hdcodex::ParseShadingMode("fused") == ShadingMode::Fused,
            "fused shading mode was not parsed");
    Require(hdcodex::ParseShadingMode("fused-specialized") == ShadingMode::Fused,
            "fused shading mode alias was not parsed");
    Require(hdcodex::ParseShadingMode("modular") == ShadingMode::Modular,
            "modular shading mode was not parsed");
    Require(hdcodex::ParseShadingMode("instrumented") == ShadingMode::Modular,
            "modular shading mode alias was not parsed");
    Require(hdcodex::ParseShadingMode("raster") == ShadingMode::RasterPreview,
            "raster shading mode was not parsed");
    Require(!hdcodex::ParseShadingMode("legacy").has_value(),
            "unknown shading mode was accepted");
    Require(hdcodex::ShadingModeName(ShadingMode::Fused) == "fused",
            "fused shading mode name changed");
    Require(hdcodex::ShadingModeName(ShadingMode::Modular) == "modular",
            "modular shading mode name changed");
    Require(hdcodex::ShadingModeName(ShadingMode::RasterPreview) == "raster",
            "raster shading mode name changed");
}

void TestCache()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "hdcodex-core-test-cache";
    std::filesystem::remove_all(root);

    const std::array<std::string, 2> options = {"-O", "--target-env=vulkan1.2"};
    const std::string key = hdcodex::MakeShaderCacheKey({
        .source = "void main() {}",
        .generatorVersion = "MaterialX-test",
        .compilerVersion = "shaderc-test",
        .targetEnvironment = "vulkan1.2",
        .materialAbi = "hdcodex-bsdf-1",
        .options = options,
    });
    const std::array payload = {
        std::byte{0x03}, std::byte{0x02}, std::byte{0x23}, std::byte{0x07}};

    hdcodex::ShaderCache cache(root);
    Require(!cache.Load(key).has_value(), "cold cache unexpectedly hit");
    cache.Store(key, payload);
    const auto loaded = cache.Load(key);
    Require(loaded.has_value(), "warm cache missed");
    Require(*loaded == std::vector<std::byte>(payload.begin(), payload.end()),
            "cached payload changed");
    Require(cache.Remove(key), "cache remove failed");
    Require(!cache.Load(key).has_value(), "removed cache entry still loads");
    std::filesystem::remove_all(root);
}

void TestVersionedScene()
{
    hdcodex::VersionedScene scene;
    Require(scene.PublishedRevision() == 0, "initial published revision is not zero");
    Require(scene.MarkDirty() == 1, "first staging revision is not one");
    Require(scene.PublishedRevision() == 0, "dirty staging leaked into published state");
    Require(scene.Publish() == 1, "published revision is incorrect");
    Require(scene.PublishedRevision() == 1, "published revision did not persist");

    hdcodex::SceneMesh mesh;
    mesh.id = "/triangle";
    mesh.positions = {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    mesh.indices = {0, 1, 2};
    mesh.materialId = "/base";
    mesh.triangleMaterialIds = {"/subset"};
    mesh.displayColor = {0.2F, 0.4F, 0.8F};
    scene.UpsertMesh(mesh);
    const auto meshRevision = scene.Publish();
    const auto snapshot = scene.Snapshot();
    Require(snapshot && snapshot->revision == meshRevision,
            "scene snapshot revision mismatch");
    Require(snapshot->meshes.size() == 1, "scene snapshot lost mesh");
    Require(snapshot->meshes.front().indices.size() == 3,
            "scene snapshot lost indices");
    Require(snapshot->meshes.front().triangleMaterialIds ==
                std::vector<std::string>{"/subset"},
            "scene snapshot lost triangle material assignments");
    Require(snapshot->meshes.front().displayColor[2] == 0.8F,
            "scene snapshot lost mesh display color");

    hdcodex::SceneMaterial material;
    material.id = "/red";
    material.baseColor = {1.0F, 0.0F, 0.0F};
    material.shaderNodeId = "ND_open_pbr_surface_surfaceshader";
    material.materialXPublicUniforms.push_back(
        {"surface_base_weight", "float", "1.0"});
    material.materialXTextures.push_back(
        {"albedo_file", "albedo#srgb", "srgb_texture"});
    material.materialXPixelDescriptors.push_back({
        .name = "PublicUniforms",
        .set = 0,
        .binding = 0,
        .kind = hdcodex::SceneMaterial::GeneratedDescriptorKind::UniformBuffer,
        .members = {{"surface_base_weight", 0}},
    });
    material.materialXOutputNode = "surface";
    material.materialXProgram.push_back({
        .name = "surface",
        .category = "surface",
        .nodeDef = "ND_surface_surfaceshader",
        .type = "surfaceshader",
        .inputs = {{
            .name = "bsdf",
            .type = "BSDF",
            .upstreamNode = "diffuse",
        }},
    });
    scene.UpsertMaterial(material);
    (void)scene.Publish();
    Require(scene.Snapshot()->materials.size() == 1,
            "scene snapshot lost material");
    Require(scene.Snapshot()->materials.front().materialXPublicUniforms.size() == 1,
            "scene snapshot lost generated MaterialX uniforms");
    Require(scene.Snapshot()->materials.front().materialXTextures.size() == 1,
            "scene snapshot lost generated MaterialX textures");
    Require(scene.Snapshot()->materials.front().materialXPixelDescriptors.size() == 1 &&
                scene.Snapshot()->materials.front()
                        .materialXPixelDescriptors.front().members.front().offset == 0,
            "scene snapshot lost generated MaterialX descriptor ABI");
    Require(scene.Snapshot()->materials.front().materialXOutputNode == "surface" &&
                scene.Snapshot()->materials.front().materialXProgram.size() == 1,
            "scene snapshot lost generated MaterialX program");
    scene.RemoveMaterial("/red");

    scene.RemoveMesh("/triangle");
    (void)scene.Publish();
    Require(scene.Snapshot()->meshes.empty(), "scene mesh removal was not published");
    Require(scene.Snapshot()->materials.empty(), "scene material removal was not published");
}

void TestSceneCarriesFaceCornerTextureCoordinates()
{
    hdcodex::VersionedScene scene;
    hdcodex::SceneMesh mesh;
    mesh.id = "/textured";
    mesh.positions = {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    mesh.indices = {0, 1, 2};
    mesh.texcoords = {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F};
    mesh.normals = {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F};
    scene.UpsertMesh(std::move(mesh));
    (void)scene.Publish();
    const auto snapshot = scene.Snapshot();
    Require(snapshot->meshes.size() == 1, "textured mesh was not published");
    Require(snapshot->meshes.front().texcoords.size() == 6,
            "face-corner texture coordinates were not preserved");
    Require(snapshot->meshes.front().normals.size() == 9,
            "face-corner normals were not preserved");
}

void TestSceneDeduplicatesDecodedTextures()
{
    hdcodex::VersionedScene scene;
    hdcodex::SceneTexture texture;
    texture.id = "albedo#srgb";
    texture.sourcePath = "albedo.png";
    texture.width = 1;
    texture.height = 1;
    texture.srgb = true;
    texture.rgba = {10, 20, 30, 255};
    scene.UpsertTexture(texture);
    Require(scene.HasTexture(texture.id), "decoded texture was not retained");
    (void)scene.Publish();
    const auto snapshot = scene.Snapshot();
    Require(snapshot->textures.size() == 1, "decoded texture was not published");
    Require(snapshot->textures.front().rgba == texture.rgba,
            "published texture pixels changed");

    hdcodex::SceneTexture environment;
    environment.id = "studio.exr#auto-hdr";
    environment.sourcePath = "studio.exr";
    environment.width = 1;
    environment.height = 1;
    environment.rgbaFloat = {4.0F, 2.0F, 1.0F, 1.0F};
    scene.UpsertTexture(environment);
    (void)scene.Publish();
    const auto hdrSnapshot = scene.Snapshot();
    Require(hdrSnapshot->textures.size() == 2,
            "HDR light texture was not published");
    Require(hdrSnapshot->textures.back().rgbaFloat[0] == 4.0F,
            "HDR light texture was clamped");
}

void TestScenePublishesLights()
{
    hdcodex::VersionedScene scene;
    hdcodex::SceneLight dome;
    dome.id = "/environment";
    dome.type = hdcodex::SceneLightType::Dome;
    dome.intensity = 2.0F;
    dome.texture = "studio.exr#auto-hdr";
    scene.UpsertLight(dome);
    (void)scene.Publish();
    const auto snapshot = scene.Snapshot();
    Require(snapshot->lights.size() == 1, "light was not published");
    Require(snapshot->lights.front().texture == dome.texture,
            "light texture binding changed");
    scene.RemoveLight(dome.id);
    (void)scene.Publish();
    Require(scene.Snapshot()->lights.empty(), "removed light remained published");
}

} // namespace

int main()
{
    try {
        TestSha256();
        TestShadingModes();
        TestCache();
        TestVersionedScene();
        TestSceneCarriesFaceCornerTextureCoordinates();
        TestSceneDeduplicatesDecodedTextures();
        TestScenePublishesLights();
        std::cout << "hdCodex core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hdCodex core tests failed: " << error.what() << '\n';
        return 1;
    }
}
