#include "hdcodex/core/shader_cache.h"
#include "hdcodex/materialx/materialx_compiler.h"

#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <ranges>

namespace {

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main()
try {
    const std::filesystem::path cacheRoot =
        std::filesystem::temp_directory_path() / "hdcodex-materialx-compiler-test";
    std::error_code error;
    std::filesystem::remove_all(cacheRoot, error);

    constexpr auto xml = R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <standard_surface name="surface" type="surfaceshader"
                    base_color="0.15, 0.35, 0.8" metalness="0.2"
                    specular_roughness="0.3"/>
  <surfacematerial name="material" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="surface"/>
  </surfacematerial>
</materialx>)mtlx";

    hdcodex::ShaderCache cache(cacheRoot);
    hdcodex::MaterialXCompiler compiler(cache);
    const auto first = compiler.CompileXml(xml, "standard_surface_test");
    Check(!first.vertexSource.empty(), "MaterialX vertex source is empty");
    Check(!first.pixelSource.empty(), "MaterialX pixel source is empty");
    Check(first.vertexSpirv.words.front() == 0x07230203u, "invalid vertex SPIR-V");
    Check(first.pixelSpirv.words.front() == 0x07230203u, "invalid pixel SPIR-V");
    Check(!first.vertexDescriptors.empty(),
          "MaterialX vertex descriptor ABI was not reflected");
    Check(!first.pixelDescriptors.empty(),
          "MaterialX pixel descriptor ABI was not reflected");
    Check(!first.program.outputNode.empty() && !first.program.nodes.empty(),
          "MaterialX closure program was not generated");
    Check(std::ranges::none_of(first.program.nodes, [](const auto& node) {
        return node.category == "standard_surface";
    }), "Standard Surface was not expanded through its MaterialX NodeGraph");
    Check(std::ranges::any_of(first.program.nodes, [](const auto& node) {
        return node.type == "BSDF";
    }), "expanded Standard Surface program contains no BSDF primitives");

    const auto second = compiler.CompileXml(xml, "standard_surface_test");
    Check(second.vertexSpirv.cacheHit, "MaterialX vertex shader cache miss");
    Check(second.pixelSpirv.cacheHit, "MaterialX pixel shader cache miss");
    Check(second.vertexSpirv.words == first.vertexSpirv.words, "cached vertex SPIR-V changed");
    Check(second.pixelSpirv.words == first.pixelSpirv.words, "cached pixel SPIR-V changed");

    const auto modular = compiler.CompileXml(
        xml, "standard_surface_test", hdcodex::ShadingMode::Modular);
    Check(modular.mode == hdcodex::ShadingMode::Modular,
          "MaterialX modular compile lost its mode");
    Check(modular.vertexSpirv.cacheKey != first.vertexSpirv.cacheKey,
          "MaterialX modular vertex shader reused the fused cache identity");
    Check(modular.pixelSpirv.cacheKey != first.pixelSpirv.cacheKey,
          "MaterialX modular pixel shader reused the fused cache identity");

    const auto raster = compiler.CompileXml(
        xml, "standard_surface_test", hdcodex::ShadingMode::RasterPreview);
    Check(raster.mode == hdcodex::ShadingMode::RasterPreview,
          "MaterialX raster compile lost its mode");
    Check(!raster.vertexSpirv.words.empty() && !raster.pixelSpirv.words.empty(),
          "MaterialX raster preview modules are empty");
    Check(raster.program.nodes.empty(),
          "raster preview unexpectedly generated a path-closure program");

    constexpr auto texturedXml = R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <geompropvalue name="st" type="vector2">
    <input name="geomprop" type="string" value="st"/>
  </geompropvalue>
  <image name="albedo" type="color3">
    <input name="file" type="filename" value="unused.png"/>
    <input name="texcoord" type="vector2" nodename="st"/>
  </image>
  <open_pbr_surface name="surface" type="surfaceshader">
    <input name="base_color" type="color3" nodename="albedo"/>
  </open_pbr_surface>
  <surfacematerial name="material" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="surface"/>
  </surfacematerial>
</materialx>)mtlx";
    const auto textured = compiler.CompileXml(texturedXml, "textured_openpbr_test");
    if (std::getenv("HDCODEX_DUMP_MATERIALX_GLSL")) {
        std::cout << "--- MATERIALX VERTEX ---\n" << textured.vertexSource
                  << "\n--- MATERIALX PIXEL ---\n" << textured.pixelSource << '\n';
    }
    Check(textured.vertexSource.find("out vec2 vd_i_geomprop_st;") != std::string::npos,
          "MaterialX vertex connector was not uniquely renamed");
    Check(textured.pixelSource.find("in vec2 vd_i_geomprop_st;") != std::string::npos,
          "MaterialX pixel connector was not uniquely renamed");
    Check(textured.vertexSource.find("vd_i_geomprop_st = i_geomprop_st;") !=
              std::string::npos,
          "MaterialX vertex connector assignment was not flattened");
    Check(std::ranges::any_of(textured.textures, [](const auto& input) {
        return input.name == "albedo_file" && input.value == "unused.png";
    }), "MaterialX texture interface was not reflected");
    Check(std::ranges::any_of(textured.publicUniforms, [](const auto& input) {
        return input.name == "surface_base_weight" && input.type == "float";
    }), "MaterialX public uniform interface was not reflected");
    Check(std::ranges::none_of(textured.program.nodes, [](const auto& node) {
        return node.category == "open_pbr_surface";
    }), "OpenPBR was not expanded through its MaterialX NodeGraph");
    Check(std::ranges::any_of(textured.program.nodes, [](const auto& node) {
        return node.category == "image";
    }), "expanded OpenPBR program lost its image node");
    Check(std::ranges::any_of(textured.program.nodes, [](const auto& node) {
        return node.category == "dielectric_bsdf";
    }), "expanded OpenPBR program contains no dielectric BSDF primitive");

    constexpr auto usdPrimvarReaderXml = R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <nodegraph name="texture_graph">
    <UsdPrimvarReader name="uv_reader" type="vector2"
                      nodedef="ND_UsdPrimvarReader_vector2">
      <input name="varname" type="string" value="st"/>
    </UsdPrimvarReader>
    <image name="albedo" type="color3" nodedef="ND_image_color3">
      <input name="file" type="filename" value="unused.png"/>
      <input name="texcoord" type="vector2" nodename="uv_reader"/>
    </image>
    <output name="base_color" type="color3" nodename="albedo"/>
  </nodegraph>
  <standard_surface name="surface" type="surfaceshader">
    <input name="base_color" type="color3"
           nodegraph="texture_graph" output="base_color"/>
  </standard_surface>
  <surfacematerial name="material" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="surface"/>
  </surfacematerial>
</materialx>)mtlx";
    const auto usdPrimvarReader = compiler.CompileXml(
        usdPrimvarReaderXml, "usd_primvar_reader_test");
    Check(usdPrimvarReader.vertexSource.find("vd_i_geomprop_st") !=
              std::string::npos,
          "MaterialX ND_UsdPrimvarReader_vector2 did not bind geomprop 'st'");
    Check(std::ranges::any_of(usdPrimvarReader.textures, [](const auto& input) {
        return input.name == "albedo_file" && input.value == "unused.png";
    }), "MaterialX USD primvar reader graph lost its image input");

    std::filesystem::remove_all(cacheRoot, error);
    std::cout << hdcodex::MaterialXCompiler::GeneratorVersion()
              << ": generation, compilation, and cache tests passed\n";
    return 0;
} catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
}
