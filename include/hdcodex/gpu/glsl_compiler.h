#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hdcodex {

class ShaderCache;

enum class GlslShaderStage {
    Vertex,
    Fragment,
    Compute,
    RayGeneration,
    AnyHit,
    ClosestHit,
    Miss,
    Intersection,
    Callable,
};

struct GlslCompileOptions {
    bool generateDebugInfo = false;
    bool optimizeForSize = false;
    std::string entryPoint = "main";
    std::string generatorVersion = "hdCodex.glsl.v1";
    std::string materialAbi = "hdcodex.bsdf.v1";
};

struct SpirvModule {
    std::vector<std::uint32_t> words;
    std::string cacheKey;
    std::string diagnostics;
    bool cacheHit = false;
};

/// In-process Vulkan GLSL compiler backed by Khronos glslang.
///
/// Successful results are persisted in ShaderCache. Compilation errors throw
/// std::runtime_error with glslang's source diagnostics.
class GlslCompiler final {
public:
    explicit GlslCompiler(ShaderCache& cache) noexcept;

    [[nodiscard]] SpirvModule Compile(
        std::string_view source,
        GlslShaderStage stage,
        std::string_view debugName,
        const GlslCompileOptions& options = {}) const;

    [[nodiscard]] static std::string Version();

private:
    ShaderCache* _cache;
};

} // namespace hdcodex
