#include "hdcodex/gpu/glsl_compiler.h"

#include "hdcodex/core/shader_cache.h"

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace hdcodex {
namespace {

constexpr std::uint32_t kSpirvMagic = 0x07230203u;

struct GlslangProcess {
    GlslangProcess()
    {
        if (glslang_initialize_process() == 0) {
            throw std::runtime_error("glslang process initialization failed");
        }
    }

    ~GlslangProcess() { glslang_finalize_process(); }
};

void EnsureGlslangInitialized()
{
    static const GlslangProcess process;
    (void)process;
}

glslang_stage_t ToGlslangStage(GlslShaderStage stage)
{
    switch (stage) {
    case GlslShaderStage::Vertex: return GLSLANG_STAGE_VERTEX;
    case GlslShaderStage::Fragment: return GLSLANG_STAGE_FRAGMENT;
    case GlslShaderStage::Compute: return GLSLANG_STAGE_COMPUTE;
    case GlslShaderStage::RayGeneration: return GLSLANG_STAGE_RAYGEN;
    case GlslShaderStage::AnyHit: return GLSLANG_STAGE_ANYHIT;
    case GlslShaderStage::ClosestHit: return GLSLANG_STAGE_CLOSESTHIT;
    case GlslShaderStage::Miss: return GLSLANG_STAGE_MISS;
    case GlslShaderStage::Intersection: return GLSLANG_STAGE_INTERSECT;
    case GlslShaderStage::Callable: return GLSLANG_STAGE_CALLABLE;
    }
    throw std::invalid_argument("unknown GLSL shader stage");
}

std::string StageName(GlslShaderStage stage)
{
    switch (stage) {
    case GlslShaderStage::Vertex: return "vertex";
    case GlslShaderStage::Fragment: return "fragment";
    case GlslShaderStage::Compute: return "compute";
    case GlslShaderStage::RayGeneration: return "raygen";
    case GlslShaderStage::AnyHit: return "anyhit";
    case GlslShaderStage::ClosestHit: return "closesthit";
    case GlslShaderStage::Miss: return "miss";
    case GlslShaderStage::Intersection: return "intersection";
    case GlslShaderStage::Callable: return "callable";
    }
    return "unknown";
}

std::string LogForShader(glslang_shader_t* shader)
{
    std::ostringstream stream;
    if (const char* log = glslang_shader_get_info_log(shader); log && *log) {
        stream << log;
    }
    if (const char* log = glslang_shader_get_info_debug_log(shader); log && *log) {
        if (stream.tellp() > 0) stream << '\n';
        stream << log;
    }
    return stream.str();
}

std::string LogForProgram(glslang_program_t* program)
{
    std::ostringstream stream;
    if (const char* log = glslang_program_get_info_log(program); log && *log) {
        stream << log;
    }
    if (const char* log = glslang_program_get_info_debug_log(program); log && *log) {
        if (stream.tellp() > 0) stream << '\n';
        stream << log;
    }
    return stream.str();
}

std::runtime_error CompileError(
    std::string_view phase,
    std::string_view debugName,
    const std::string& log)
{
    std::ostringstream stream;
    stream << "GLSL " << phase << " failed for " << debugName;
    if (!log.empty()) stream << ":\n" << log;
    return std::runtime_error(stream.str());
}

} // namespace

GlslCompiler::GlslCompiler(ShaderCache& cache) noexcept : _cache(&cache) {}

std::string GlslCompiler::Version()
{
    EnsureGlslangInitialized();
    glslang_version_t version{};
    glslang_get_version(&version);
    std::ostringstream stream;
    stream << version.major << '.' << version.minor << '.' << version.patch;
    if (version.flavor && *version.flavor) stream << '-' << version.flavor;
    return stream.str();
}

SpirvModule GlslCompiler::Compile(
    std::string_view source,
    GlslShaderStage stage,
    std::string_view debugName,
    const GlslCompileOptions& options) const
{
    EnsureGlslangInitialized();
    if (source.empty()) throw std::invalid_argument("GLSL source must not be empty");
    if (options.entryPoint.empty()) throw std::invalid_argument("GLSL entry point must not be empty");

    const char* optimization = "optperformance";
    if (options.optimization == GlslCompileOptions::Optimization::None) {
        optimization = "noopt";
    } else if (options.optimization == GlslCompileOptions::Optimization::Size) {
        optimization = "optsize";
    }
    const std::array<std::string, 4> keyOptions = {
        StageName(stage),
        options.entryPoint,
        options.generateDebugInfo ? "debug" : "nodebug",
        optimization,
    };
    const std::string compilerVersion = Version();
    const ShaderCacheKeyInput keyInput{
        .source = source,
        .generatorVersion = options.generatorVersion,
        .compilerVersion = compilerVersion,
        .targetEnvironment = "vulkan1.3-spirv1.6",
        .materialAbi = options.materialAbi,
        .options = keyOptions,
    };
    const std::string cacheKey = MakeShaderCacheKey(keyInput);

    if (const auto cached = _cache->Load(cacheKey)) {
        if (cached->size() >= sizeof(kSpirvMagic) &&
            cached->size() % sizeof(std::uint32_t) == 0) {
            SpirvModule module;
            module.words.resize(cached->size() / sizeof(std::uint32_t));
            std::memcpy(module.words.data(), cached->data(), cached->size());
            if (module.words.front() == kSpirvMagic) {
                module.cacheKey = cacheKey;
                module.cacheHit = true;
                return module;
            }
        }
        (void)_cache->Remove(cacheKey);
    }

    const std::string terminatedSource(source);
    const glslang_stage_t nativeStage = ToGlslangStage(stage);
    const glslang_input_t input{
        .language = GLSLANG_SOURCE_GLSL,
        .stage = nativeStage,
        .client = GLSLANG_CLIENT_VULKAN,
        .client_version = GLSLANG_TARGET_VULKAN_1_3,
        .target_language = GLSLANG_TARGET_SPV,
        .target_language_version = GLSLANG_TARGET_SPV_1_6,
        .code = terminatedSource.c_str(),
        .default_version = 460,
        .default_profile = GLSLANG_CORE_PROFILE,
        .force_default_version_and_profile = false,
        .forward_compatible = false,
        .messages = static_cast<glslang_messages_t>(
            GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT),
        .resource = glslang_default_resource(),
    };

    using ShaderPtr = std::unique_ptr<glslang_shader_t, decltype(&glslang_shader_delete)>;
    ShaderPtr shader(glslang_shader_create(&input), &glslang_shader_delete);
    if (!shader) throw std::runtime_error("glslang failed to allocate a shader");
    glslang_shader_set_entry_point(shader.get(), options.entryPoint.c_str());

    if (glslang_shader_preprocess(shader.get(), &input) == 0) {
        throw CompileError("preprocessing", debugName, LogForShader(shader.get()));
    }
    if (glslang_shader_parse(shader.get(), &input) == 0) {
        throw CompileError("parsing", debugName, LogForShader(shader.get()));
    }

    using ProgramPtr = std::unique_ptr<glslang_program_t, decltype(&glslang_program_delete)>;
    ProgramPtr program(glslang_program_create(), &glslang_program_delete);
    if (!program) throw std::runtime_error("glslang failed to allocate a program");
    glslang_program_add_shader(program.get(), shader.get());
    const int linkMessages = GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT;
    if (glslang_program_link(program.get(), linkMessages) == 0) {
        throw CompileError("linking", debugName, LogForProgram(program.get()));
    }

    glslang_spv_options_t spvOptions{};
    spvOptions.generate_debug_info = options.generateDebugInfo;
    spvOptions.strip_debug_info = !options.generateDebugInfo;
    spvOptions.disable_optimizer =
        options.optimization == GlslCompileOptions::Optimization::None;
    spvOptions.optimize_size =
        options.optimization == GlslCompileOptions::Optimization::Size;
    spvOptions.validate = true;
    glslang_program_SPIRV_generate_with_options(program.get(), nativeStage, &spvOptions);

    SpirvModule module;
    module.words.resize(glslang_program_SPIRV_get_size(program.get()));
    if (module.words.empty()) {
        throw CompileError("SPIR-V generation", debugName, LogForProgram(program.get()));
    }
    glslang_program_SPIRV_get(program.get(), module.words.data());
    if (module.words.front() != kSpirvMagic) {
        throw std::runtime_error("glslang returned an invalid SPIR-V module");
    }
    if (const char* messages = glslang_program_SPIRV_get_messages(program.get());
        messages && *messages) {
        module.diagnostics = messages;
    }

    const auto bytes = std::span{
        reinterpret_cast<const std::byte*>(module.words.data()),
        module.words.size() * sizeof(std::uint32_t)};
    _cache->Store(cacheKey, bytes);
    module.cacheKey = cacheKey;
    return module;
}

} // namespace hdcodex
