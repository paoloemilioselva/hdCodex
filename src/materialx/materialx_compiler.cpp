#include "hdcodex/materialx/materialx_compiler.h"

#include "pxr/imaging/hdMtlx/hdMtlx.h"

#include <MaterialXCore/Util.h>
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXGenGlsl/VkShaderGenerator.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/HwShaderGenerator.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>

#include <stdexcept>
#include <string>

namespace hdcodex {
namespace {

MaterialX::FileSearchPath MaterialXSearchPaths()
{
    MaterialX::FileSearchPath paths = pxr::HdMtlxSearchPaths();
    paths.prepend(MaterialX::FilePath(HDCODEX_MATERIALX_DATA_ROOT));
    return paths;
}

void ReplaceAll(std::string& source, std::string_view needle, std::string_view replacement)
{
    if (needle.empty()) return;
    std::size_t position = 0;
    while ((position = source.find(needle, position)) != std::string::npos) {
        source.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
}

void RenameInterfaceDeclaration(
    std::string& source,
    std::string_view qualifier,
    std::string_view oldName,
    std::string_view newName)
{
    const std::string qualifierMarker = " " + std::string(qualifier) + " ";
    const std::string declarationEnd = " " + std::string(oldName) + ";";
    std::size_t lineStart = 0;
    while (lineStart < source.size()) {
        const std::size_t lineEnd = source.find('\n', lineStart);
        const std::size_t length = (lineEnd == std::string::npos ? source.size() : lineEnd) -
            lineStart;
        const std::string_view line(source.data() + lineStart, length);
        if (line.find("layout") != std::string_view::npos &&
            line.find(qualifierMarker) != std::string_view::npos &&
            line.ends_with(declarationEnd)) {
            source.replace(lineStart + length - oldName.size() - 1,
                           oldName.size(), newName);
            lineStart += newName.size() - oldName.size();
        }
        if (lineEnd == std::string::npos) break;
        lineStart = source.find('\n', lineStart) + 1;
    }
}

void FlattenVulkanVertexData(MaterialX::Shader& shader, std::string& vertex, std::string& pixel)
{
    // MaterialX 1.39.3's Vk generator declares stage connectors as individual
    // location-qualified variables, but still emits the struct instance prefix
    // inherited from the desktop GLSL generator. Flatten the prefix while
    // retaining a distinct connector namespace: an output such as
    // i_geomprop_st otherwise collides with the vertex input of the same name.
    const auto& block = shader.getStage(MaterialX::Stage::VERTEX)
        .getOutputBlock(MaterialX::HW::VERTEX_DATA);
    if (block.empty()) return;
    for (const MaterialX::ShaderPort* port : block.getVariableOrder()) {
        const std::string& name = port->getVariable();
        const std::string flattenedName = "vd_" + name;
        const std::string qualifiedName =
            MaterialX::HW::VERTEX_DATA_INSTANCE + "." + name;
        RenameInterfaceDeclaration(vertex, "out", name, flattenedName);
        RenameInterfaceDeclaration(pixel, "in", name, flattenedName);
        ReplaceAll(vertex, qualifiedName, flattenedName);
        ReplaceAll(pixel, qualifiedName, flattenedName);
    }
}

} // namespace

MaterialXCompiler::MaterialXCompiler(ShaderCache& cache) noexcept : _glsl(cache) {}

std::string MaterialXCompiler::GeneratorVersion()
{
    return "MaterialX-" + MaterialX::getVersionString() + "-" +
        MaterialX::VkShaderGenerator::TARGET + "-" +
        MaterialX::VkShaderGenerator::VERSION;
}

MaterialXCompiledShader MaterialXCompiler::CompileXml(
    std::string_view xml,
    std::string_view shaderName) const
{
    if (xml.empty()) throw std::invalid_argument("MaterialX XML must not be empty");
    auto document = MaterialX::createDocument();
    const std::string terminatedXml(xml);
    MaterialX::readFromXmlBuffer(
        document, terminatedXml.c_str(), MaterialXSearchPaths());
    document->setDataLibrary(pxr::HdMtlxStdLibraries());
    return CompileDocument(document, shaderName);
}

MaterialXCompiledShader MaterialXCompiler::CompileDocument(
    const MaterialX::DocumentPtr& document,
    std::string_view shaderName) const
{
    if (!document) throw std::invalid_argument("MaterialX document must not be null");

    const auto renderables = MaterialX::findRenderableElements(document);
    if (renderables.empty()) {
        throw std::runtime_error("MaterialX document has no renderable element");
    }

    const auto generator = MaterialX::VkShaderGenerator::create();
    MaterialX::GenContext context(generator);
    context.registerSourceCodeSearchPath(MaterialXSearchPaths());
    generator->registerTypeDefs(document);

    const std::string validName = MaterialX::createValidName(
        shaderName.empty() ? renderables.front()->getName() : std::string(shaderName));
    const auto shader = generator->generate(validName, renderables.front(), context);
    if (!shader) throw std::runtime_error("MaterialX Vulkan shader generation returned null");

    MaterialXCompiledShader result;
    result.name = validName;
    result.vertexSource = shader->getSourceCode(MaterialX::Stage::VERTEX);
    result.pixelSource = shader->getSourceCode(MaterialX::Stage::PIXEL);
    FlattenVulkanVertexData(*shader, result.vertexSource, result.pixelSource);
    if (result.vertexSource.empty() || result.pixelSource.empty()) {
        throw std::runtime_error("MaterialX generated an empty Vulkan shader stage");
    }

    GlslCompileOptions options;
    options.generatorVersion = GeneratorVersion();
    options.materialAbi = "hdcodex.materialx-raster.v1";
    try {
        result.vertexSpirv = _glsl.Compile(
            result.vertexSource, GlslShaderStage::Vertex, validName + ".vert", options);
    } catch (const std::runtime_error& error) {
        throw std::runtime_error(std::string(error.what()) +
            "\nGenerated MaterialX vertex source:\n" + result.vertexSource);
    }
    try {
        result.pixelSpirv = _glsl.Compile(
            result.pixelSource, GlslShaderStage::Fragment, validName + ".frag", options);
    } catch (const std::runtime_error& error) {
        throw std::runtime_error(std::string(error.what()) +
            "\nGenerated MaterialX pixel source:\n" + result.pixelSource);
    }
    return result;
}

} // namespace hdcodex
