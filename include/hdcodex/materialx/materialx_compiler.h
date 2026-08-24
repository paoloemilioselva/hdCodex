#pragma once

#include "hdcodex/gpu/glsl_compiler.h"

#include <MaterialXCore/Document.h>

#include <string>
#include <string_view>

namespace hdcodex {

class ShaderCache;

struct MaterialXCompiledShader {
    std::string name;
    std::string vertexSource;
    std::string pixelSource;
    SpirvModule vertexSpirv;
    SpirvModule pixelSpirv;
};

/// Generates Vulkan GLSL from MaterialX and compiles both stages to cached SPIR-V.
class MaterialXCompiler final {
public:
    explicit MaterialXCompiler(ShaderCache& cache) noexcept;

    [[nodiscard]] MaterialXCompiledShader CompileDocument(
        const MaterialX::DocumentPtr& document,
        std::string_view shaderName) const;

    [[nodiscard]] MaterialXCompiledShader CompileXml(
        std::string_view xml,
        std::string_view shaderName) const;

    [[nodiscard]] static std::string GeneratorVersion();

private:
    GlslCompiler _glsl;
};

} // namespace hdcodex
