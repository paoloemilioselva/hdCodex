#include "hdcodex/core/shader_cache.h"
#include "hdcodex/materialx/materialx_compiler.h"

#include <filesystem>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <ranges>

namespace {

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void DumpProgram(const hdcodex::MaterialXCompiledShader& shader)
{
    const bool dumpProgram = std::getenv("HDCODEX_DUMP_MATERIALX_PROGRAM") != nullptr;
    if (!dumpProgram) return;
    std::cout << "--- MATERIALX PROGRAM " << shader.name << " -> "
              << shader.program.outputNode << " ---\n";
    for (const auto& node : shader.program.nodes) {
        std::cout << node.name << " category=" << node.category
                  << " nodedef=" << node.nodeDef << " type=" << node.type << '\n';
        for (const auto& input : node.inputs) {
            std::cout << "  " << input.name << ':' << input.type;
            if (!input.upstreamNode.empty()) {
                std::cout << " <- " << input.upstreamNode;
                if (!input.upstreamOutput.empty()) {
                    std::cout << '.' << input.upstreamOutput;
                }
            } else {
                std::cout << " = " << input.value;
            }
            std::cout << '\n';
        }
    }
    if (shader.closure) {
        std::cout << "closure baseColor=" << shader.closure->baseColor[0] << ','
                  << shader.closure->baseColor[1] << ','
                  << shader.closure->baseColor[2]
                  << " metalness=" << shader.closure->metalness
                  << " roughness=" << shader.closure->roughness << '\n';
    }
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
  <standard_surface name="surface" type="surfaceshader">
    <input name="base_color" type="color3" value="0.15, 0.35, 0.8"/>
    <input name="metalness" type="float" value="0.2"/>
    <input name="specular_roughness" type="float" value="0.3"/>
  </standard_surface>
  <surfacematerial name="material" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="surface"/>
  </surfacematerial>
</materialx>)mtlx";

    hdcodex::ShaderCache cache(cacheRoot);
    hdcodex::MaterialXCompiler compiler(cache);
    const auto first = compiler.CompileXml(xml, "standard_surface_test");
    DumpProgram(first);
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
    Check(first.closure.has_value(),
          "expanded Standard Surface did not compile to a closure ABI");
    Check(std::abs(first.closure->baseColor[0] - 0.15F) < 1e-5F &&
              std::abs(first.closure->baseColor[2] - 0.8F) < 1e-5F,
          "closure ABI did not derive base color from the expanded graph");
    Check(std::abs(first.closure->metalness - 0.2F) < 1e-5F &&
              std::abs(first.closure->roughness - 0.3F) < 1e-5F,
          "closure ABI did not derive metalness and roughness from graph combiners");

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
    DumpProgram(textured);
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
    Check(textured.closure && textured.closure->baseColorTexture == "unused.png",
          "closure ABI did not retain the MaterialX image driving base color");

    constexpr auto primitiveClosureXml = R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <oren_nayar_diffuse_bsdf name="diffuse" type="BSDF">
    <input name="color" type="color3" value="0.2, 0.4, 0.6"/>
    <input name="roughness" type="float" value="0"/>
  </oren_nayar_diffuse_bsdf>
  <uniform_edf name="emission" type="EDF">
    <input name="color" type="color3" value="0.01, 0.02, 0.03"/>
  </uniform_edf>
  <surface name="surface" type="surfaceshader">
    <input name="bsdf" type="BSDF" nodename="diffuse"/>
    <input name="edf" type="EDF" nodename="emission"/>
    <input name="opacity" type="float" value="0.75"/>
  </surface>
  <surfacematerial name="material" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="surface"/>
  </surfacematerial>
</materialx>)mtlx";
    const auto primitiveClosure = compiler.CompileXml(
        primitiveClosureXml, "primitive_closure_test");
    Check(primitiveClosure.closure &&
              std::abs(primitiveClosure.closure->baseColor[1] - 0.4F) < 1e-5F &&
              std::abs(primitiveClosure.closure->emission[2] - 0.03F) < 1e-5F &&
              std::abs(primitiveClosure.closure->opacity - 0.75F) < 1e-5F,
          "primitive MaterialX closure did not drive the renderer ABI");

    const hdcodex::MaterialXGeneratedProgram endpointMetalProgram{
        .outputNode = "surface",
        .nodes = {
            {.name = "diffuse", .category = "oren_nayar_diffuse_bsdf",
             .nodeDef = "ND_oren_nayar_diffuse_bsdf", .type = "BSDF",
             .inputs = {
                 {.name = "color", .type = "color3", .value = "0.2,0.3,0.4"},
                 {.name = "roughness", .type = "float", .value = "0"}}},
            {.name = "metal", .category = "generalized_schlick_bsdf",
             .nodeDef = "ND_generalized_schlick_bsdf", .type = "BSDF",
             .inputs = {
                 {.name = "weight", .type = "float", .value = "1"},
                 {.name = "color0", .type = "color3", .value = "0.9,0.7,0.2"},
                 {.name = "roughness", .type = "float", .value = "0.05"}}},
            {.name = "metal_mix", .category = "mix", .nodeDef = "ND_mix_bsdf",
             .type = "BSDF", .inputs = {
                 {.name = "fg", .type = "BSDF", .upstreamNode = "metal"},
                 {.name = "bg", .type = "BSDF", .upstreamNode = "diffuse"},
                 {.name = "mix", .type = "float", .value = "1"}}},
            {.name = "surface", .category = "surface", .nodeDef = "ND_surface",
             .type = "surfaceshader", .inputs = {
                 {.name = "bsdf", .type = "BSDF", .upstreamNode = "metal_mix"}}}
        }};
    const auto endpointMetal = hdcodex::CompileMaterialXClosure(endpointMetalProgram);
    Check(std::abs(endpointMetal.metalness - 1.0F) < 1e-5F &&
              std::abs(endpointMetal.baseColor[0] - 0.9F) < 1e-5F,
          "endpoint MaterialX conductor mix lost its metalness semantic");

    const hdcodex::MaterialXGeneratedProgram reconstructedNormalProgram{
        .outputNode = "surface",
        .nodes = {
            {.name = "normal_image", .category = "image", .nodeDef = "ND_image_vector3",
             .type = "vector3", .inputs = {
                 {.name = "file", .type = "filename", .value = "normal.png"}}},
            {.name = "red", .category = "extract", .nodeDef = "ND_extract_vector3",
             .type = "float", .inputs = {
                 {.name = "in", .type = "vector3", .upstreamNode = "normal_image"},
                 {.name = "index", .type = "integer", .value = "0"}}},
            {.name = "green", .category = "extract", .nodeDef = "ND_extract_vector3",
             .type = "float", .inputs = {
                 {.name = "in", .type = "vector3", .upstreamNode = "normal_image"},
                 {.name = "index", .type = "integer", .value = "1"}}},
            {.name = "green_inverted", .category = "invert", .nodeDef = "ND_invert_float",
             .type = "float", .inputs = {
                 {.name = "in", .type = "float", .upstreamNode = "green"}}},
            {.name = "blue", .category = "extract", .nodeDef = "ND_extract_vector3",
             .type = "float", .inputs = {
                 {.name = "in", .type = "vector3", .upstreamNode = "normal_image"},
                 {.name = "index", .type = "integer", .value = "2"}}},
            {.name = "combined", .category = "combine3", .nodeDef = "ND_combine3_vector3",
             .type = "vector3", .inputs = {
                 {.name = "in1", .type = "float", .upstreamNode = "red"},
                 {.name = "in2", .type = "float", .upstreamNode = "green_inverted"},
                 {.name = "in3", .type = "float", .upstreamNode = "blue"}}},
            {.name = "normal_map", .category = "normalmap", .nodeDef = "ND_normalmap_vector2",
             .type = "vector3", .inputs = {
                 {.name = "in", .type = "vector3", .upstreamNode = "combined"},
                 {.name = "scale", .type = "float", .value = "0.25"}}},
            {.name = "diffuse", .category = "oren_nayar_diffuse_bsdf",
             .nodeDef = "ND_oren_nayar_diffuse_bsdf", .type = "BSDF", .inputs = {
                 {.name = "color", .type = "color3", .value = "0.5,0.5,0.5"},
                 {.name = "roughness", .type = "float", .value = "0"},
                 {.name = "normal", .type = "vector3", .upstreamNode = "normal_map"}}},
            {.name = "surface", .category = "surface", .nodeDef = "ND_surface",
             .type = "surfaceshader", .inputs = {
                 {.name = "bsdf", .type = "BSDF", .upstreamNode = "diffuse"}}}
        }};
    const auto reconstructedNormal =
        hdcodex::CompileMaterialXClosure(reconstructedNormalProgram);
    Check(reconstructedNormal.normalTexture == "normal.png" &&
              reconstructedNormal.normalTextureFlipY &&
              std::abs(reconstructedNormal.normalTextureScale - 0.25F) < 1e-5F,
          "MaterialX reconstructed normal map lost its image, Y inversion, or scale");

    constexpr auto defaultImageXml = R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <image name="optional_image" type="color3">
    <input name="default" type="color3" value="0.25, 0.5, 0.75"/>
  </image>
  <oren_nayar_diffuse_bsdf name="diffuse" type="BSDF">
    <input name="color" type="color3" nodename="optional_image"/>
    <input name="roughness" type="float" value="0"/>
  </oren_nayar_diffuse_bsdf>
  <surface name="surface" type="surfaceshader">
    <input name="bsdf" type="BSDF" nodename="diffuse"/>
  </surface>
  <surfacematerial name="material" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="surface"/>
  </surfacematerial>
</materialx>)mtlx";
    const auto defaultImage = compiler.CompileXml(
        defaultImageXml, "default_image_test");
    Check(defaultImage.closure &&
              std::abs(defaultImage.closure->baseColor[0] - 0.25F) < 1e-5F &&
              std::abs(defaultImage.closure->baseColor[2] - 0.75F) < 1e-5F,
          "MaterialX image without a file did not use its authored default");

    constexpr auto unsupportedSheenXml = R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <sheen_bsdf name="sheen" type="BSDF">
    <input name="weight" type="float" value="1"/>
    <input name="color" type="color3" value="1, 0, 0"/>
    <input name="roughness" type="float" value="0.5"/>
  </sheen_bsdf>
  <surface name="surface" type="surfaceshader">
    <input name="bsdf" type="BSDF" nodename="sheen"/>
  </surface>
  <surfacematerial name="material" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="surface"/>
  </surfacematerial>
</materialx>)mtlx";
    bool unsupportedSheenRejected = false;
    try {
        (void)compiler.CompileXml(unsupportedSheenXml, "unsupported_sheen_test");
    } catch (const std::runtime_error& error) {
        unsupportedSheenRejected =
            std::string_view(error.what()).find("active sheen closure") !=
            std::string_view::npos;
    }
    Check(unsupportedSheenRejected,
          "active unsupported MaterialX sheen was not rejected explicitly");

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
