#include "hdcodex/core/hash.h"
#include "hdcodex/core/shader_cache.h"
#include "hdcodex/core/versioned_scene.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

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
}

} // namespace

int main()
{
    try {
        TestSha256();
        TestCache();
        TestVersionedScene();
        std::cout << "hdCodex core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hdCodex core tests failed: " << error.what() << '\n';
        return 1;
    }
}

