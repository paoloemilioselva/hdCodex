#include "hdcodex/core/shader_cache.h"
#include "hdcodex/gpu/glsl_compiler.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main()
try {
    const std::filesystem::path cacheRoot =
        std::filesystem::temp_directory_path() / "hdcodex-glsl-compiler-test";
    std::error_code error;
    std::filesystem::remove_all(cacheRoot, error);

    hdcodex::ShaderCache cache(cacheRoot);
    hdcodex::GlslCompiler compiler(cache);
    constexpr auto source = R"glsl(
#version 460
#extension GL_EXT_ray_query : require
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform accelerationStructureEXT scene;
layout(set = 0, binding = 1, rgba32f) uniform image2D outputImage;
void main()
{
    rayQueryEXT query;
    rayQueryInitializeEXT(query, scene, gl_RayFlagsOpaqueEXT, 0xff,
        vec3(0.0), 0.001, vec3(0.0, 0.0, -1.0), 10000.0);
    while (rayQueryProceedEXT(query)) {}
    imageStore(outputImage, ivec2(gl_GlobalInvocationID.xy), vec4(0.0));
}
)glsl";

    const auto first = compiler.Compile(source, hdcodex::GlslShaderStage::Compute, "ray-query-test.comp");
    Check(!first.cacheHit, "first compilation unexpectedly hit the cache");
    Check(!first.words.empty() && first.words.front() == 0x07230203u, "invalid SPIR-V output");

    const auto second = compiler.Compile(source, hdcodex::GlslShaderStage::Compute, "ray-query-test.comp");
    Check(second.cacheHit, "second compilation did not hit the cache");
    Check(second.cacheKey == first.cacheKey, "cache key changed between identical compilations");
    Check(second.words == first.words, "cached SPIR-V differs from compiled output");

    bool invalidSourceRejected = false;
    try {
        (void)compiler.Compile("#version 460\nthis is invalid;", hdcodex::GlslShaderStage::Compute, "invalid.comp");
    } catch (const std::runtime_error&) {
        invalidSourceRejected = true;
    }
    Check(invalidSourceRejected, "invalid GLSL was not rejected");

    std::filesystem::remove_all(cacheRoot, error);
    std::cout << "glslang " << hdcodex::GlslCompiler::Version()
              << ": compile and persistent cache tests passed\n";
    return 0;
} catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
}
