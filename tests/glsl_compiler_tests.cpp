#include "hdcodex/core/shader_cache.h"
#include "hdcodex/gpu/glsl_compiler.h"
#include "hdcodex/gpu/spirv_reflection.h"

#include <algorithm>
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
layout(std140, set = 1, binding = 4) uniform ReflectedBlock
{
    float weight;
    vec3 color;
} reflected;
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
    const auto descriptors = hdcodex::ReflectSpirvDescriptors(first.words);
    const auto reflected = std::ranges::find_if(descriptors, [](const auto& value) {
        return value.set == 1U && value.binding == 4U;
    });
    Check(reflected != descriptors.end(), "uniform descriptor was not reflected");
    Check(reflected->kind == hdcodex::SpirvDescriptorKind::UniformBuffer,
          "uniform descriptor kind is incorrect");
    Check(reflected->members.size() == 2U &&
              reflected->members[0].name == "weight" &&
              reflected->members[0].offset == 0U &&
              reflected->members[1].name == "color" &&
              reflected->members[1].offset == 16U,
          "std140 member offsets were not reflected");

    const auto second = compiler.Compile(source, hdcodex::GlslShaderStage::Compute, "ray-query-test.comp");
    Check(second.cacheHit, "second compilation did not hit the cache");
    Check(second.cacheKey == first.cacheKey, "cache key changed between identical compilations");
    Check(second.words == first.words, "cached SPIR-V differs from compiled output");

    hdcodex::GlslCompileOptions unoptimized;
    unoptimized.optimization = hdcodex::GlslCompileOptions::Optimization::None;
    const auto third = compiler.Compile(
        source, hdcodex::GlslShaderStage::Compute, "ray-query-test.comp", unoptimized);
    Check(!third.cacheHit, "different optimization mode unexpectedly hit the cache");
    Check(third.cacheKey != first.cacheKey,
          "optimization mode was not included in the cache key");

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
