#pragma once

#include "hdcodex/core/shading_mode.h"
#include "hdcodex/core/versioned_scene.h"
#include "hdcodex/gpu/glsl_compiler.h"
#include "hdcodex/gpu/spirv_reflection.h"

#include <MaterialXCore/Document.h>

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace hdcodex {

class ShaderCache;

/// Reflected MaterialX shader input.  Values are kept in MaterialX's canonical
/// string form so the renderer can pack them for the target ABI without
/// reverse-engineering generated GLSL declarations.
struct MaterialXShaderInput {
    std::string name;
    std::string type;
    std::string value;
    std::string colorSpace;
};

/// One input in the fully expanded MaterialX execution graph. Connections use
/// stable MaterialX name paths instead of renderer-specific shader fields.
struct MaterialXProgramInput {
    std::string name;
    std::string type;
    std::string value;
    std::string upstreamNode;
    std::string upstreamOutput;
    std::string colorSpace;
};

/// A MaterialX node after NodeGraph implementations (including OpenPBR and
/// Standard Surface) have been expanded by MaterialX itself. Nodes are stored
/// in dependency order so a closure target can generate or interpret them
/// without recognizing the original high-level surface model.
struct MaterialXProgramNode {
    std::string name;
    std::string category;
    std::string nodeDef;
    std::string type;
    std::vector<MaterialXProgramInput> inputs;
};

struct MaterialXGeneratedProgram {
    std::string outputNode;
    std::vector<MaterialXProgramNode> nodes;
};

struct MaterialXCompiledShader {
    ShadingMode mode{ShadingMode::Fused};
    std::string name;
    std::string vertexSource;
    std::string pixelSource;
    SpirvModule vertexSpirv;
    SpirvModule pixelSpirv;
    std::vector<SpirvDescriptor> vertexDescriptors;
    std::vector<SpirvDescriptor> pixelDescriptors;
    std::vector<MaterialXShaderInput> publicUniforms;
    std::vector<MaterialXShaderInput> textures;
    MaterialXGeneratedProgram program;
    MaterialXGeneratedProgram displacementProgram;
    /// Renderer closure ABI compiled exclusively from the expanded MaterialX
    /// program. Empty for raster preview or when no path program is requested.
    std::optional<SceneMaterial> closure;
};

struct MaterialXEvaluationContext {
    std::array<float, 2> texcoord{};
    std::array<float, 3> position{};
    std::array<float, 3> normal{0.0F, 0.0F, 1.0F};
    std::array<float, 3> tangent{1.0F, 0.0F, 0.0F};
    std::array<float, 3> bitangent{0.0F, 1.0F, 0.0F};
    std::function<std::array<float, 4>(
        std::string_view, std::string_view, const std::array<float, 2>&)>
        sampleTexture;
};

struct MaterialXDisplacement {
    /// Scalar displacement is returned in vector[2] and follows the normal.
    /// Vector displacement uses all three tangent/bitangent/normal components.
    std::array<float, 3> vector{};
    bool tangentSpace{false};
};

[[nodiscard]] MaterialXDisplacement EvaluateMaterialXDisplacement(
    const MaterialXGeneratedProgram& program,
    const MaterialXEvaluationContext& context);

/// Compiles an expanded MaterialX program into the currently supported
/// renderer closure ABI. The compiler recognizes only primitive MaterialX
/// NodeDefs and generic graph combiners; high-level shader names are never
/// inspected. Unsupported terminal closure semantics remain errors; auxiliary
/// decorations that the compact ABI cannot encode preserve the supported base.
[[nodiscard]] SceneMaterial CompileMaterialXClosure(
    const MaterialXGeneratedProgram& program,
    std::string_view terminalNodeDef = {});

/// Generates Vulkan GLSL from MaterialX and compiles both stages to cached SPIR-V.
class MaterialXCompiler final {
public:
    explicit MaterialXCompiler(ShaderCache& cache) noexcept;

    [[nodiscard]] MaterialXCompiledShader CompileDocument(
        const MaterialX::DocumentPtr& document,
        std::string_view shaderName,
        ShadingMode mode = ShadingMode::Fused) const;

    [[nodiscard]] MaterialXCompiledShader CompileXml(
        std::string_view xml,
        std::string_view shaderName,
        ShadingMode mode = ShadingMode::Fused) const;

    /// Builds the dependency-ordered program for a specific MaterialX shader
    /// terminal without compiling a raster pipeline.
    [[nodiscard]] MaterialXGeneratedProgram CompileProgram(
        const MaterialX::DocumentPtr& document,
        std::string_view outputType) const;

    [[nodiscard]] static std::string GeneratorVersion();

private:
    GlslCompiler _glsl;
};

} // namespace hdcodex
