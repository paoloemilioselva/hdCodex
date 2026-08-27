#pragma once

#include "hdcodex/core/shading_mode.h"
#include "hdcodex/gpu/glsl_compiler.h"
#include "hdcodex/gpu/spirv_reflection.h"

#include <MaterialXCore/Document.h>

#include <string>
#include <string_view>
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
};

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

    [[nodiscard]] static std::string GeneratorVersion();

private:
    GlslCompiler _glsl;
};

} // namespace hdcodex
