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
    <input name="file" type="filename" value="unused.png" colorspace="srgb_tx"/>
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
        return input.name == "albedo_file" && input.value == "unused.png" &&
            input.colorSpace == "srgb_texture";
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
    <input name="weight" type="float" value="0.6"/>
    <input name="color" type="color3" value="0.2, 0.4, 0.6"/>
    <input name="roughness" type="float" value="0.35"/>
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
              std::abs(primitiveClosure.closure->opacity - 0.75F) < 1e-5F &&
              std::abs(primitiveClosure.closure->diffuseWeight - 0.6F) < 1e-5F &&
              std::abs(primitiveClosure.closure->diffuseRoughness - 0.35F) < 1e-5F &&
              primitiveClosure.closure->diffuseModel ==
                  hdcodex::SceneMaterial::DiffuseModel::OrenNayar,
          "primitive MaterialX closure did not drive the renderer ABI");

    constexpr auto burleyDiffuseXml = R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <burley_diffuse_bsdf name="diffuse" type="BSDF">
    <input name="color" type="color3" value="0.7, 0.5, 0.3"/>
    <input name="roughness" type="float" value="0.65"/>
  </burley_diffuse_bsdf>
  <surface name="surface" type="surfaceshader">
    <input name="bsdf" type="BSDF" nodename="diffuse"/>
  </surface>
  <surfacematerial name="material" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="surface"/>
  </surfacematerial>
</materialx>)mtlx";
    const auto burleyDiffuse = compiler.CompileXml(
        burleyDiffuseXml, "burley_diffuse_test");
    Check(burleyDiffuse.closure &&
              burleyDiffuse.closure->diffuseModel ==
                  hdcodex::SceneMaterial::DiffuseModel::Burley &&
              std::abs(burleyDiffuse.closure->diffuseRoughness - 0.65F) < 1e-5F,
          "MaterialX Burley diffuse primitive lost its model or roughness");

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

    const hdcodex::MaterialXGeneratedProgram translucentProgram{
        .outputNode = "surface",
        .nodes = {
            {.name = "diffuse", .category = "oren_nayar_diffuse_bsdf",
             .nodeDef = "ND_oren_nayar_diffuse_bsdf", .type = "BSDF",
             .inputs = {
                 {.name = "weight", .type = "float", .value = "1"},
                 {.name = "color", .type = "color3", .value = "0.1,0.2,0.3"},
                 {.name = "roughness", .type = "float", .value = "0"}}},
            {.name = "translucent", .category = "translucent_bsdf",
             .nodeDef = "ND_translucent_bsdf", .type = "BSDF",
             .inputs = {
                 {.name = "weight", .type = "float", .value = "0.8"},
                 {.name = "color", .type = "color3", .value = "0.9,0.2,0.1"}}},
            {.name = "mix", .category = "mix", .nodeDef = "ND_mix_bsdf",
             .type = "BSDF", .inputs = {
                 {.name = "fg", .type = "BSDF", .upstreamNode = "translucent"},
                 {.name = "bg", .type = "BSDF", .upstreamNode = "diffuse"},
                 {.name = "mix", .type = "float", .value = "0.25"}}},
            {.name = "surface", .category = "surface", .nodeDef = "ND_surface",
             .type = "surfaceshader", .inputs = {
                 {.name = "bsdf", .type = "BSDF", .upstreamNode = "mix"}}},
        }};
    const auto translucent =
        hdcodex::CompileMaterialXClosure(translucentProgram);
    Check(std::abs(translucent.translucentWeight - 0.2F) < 1e-5F &&
              std::abs(translucent.translucentColor[0] - 0.9F) < 1e-5F &&
              std::abs(translucent.diffuseWeight - 1.0F) < 1e-5F &&
              std::abs(translucent.subsurface) < 1e-5F &&
              !translucent.thinWalled,
          "MaterialX translucent BSDF was not kept independent from subsurface");
    auto inverseTranslucentProgram = translucentProgram;
    inverseTranslucentProgram.nodes[2].inputs[0].upstreamNode = "diffuse";
    inverseTranslucentProgram.nodes[2].inputs[1].upstreamNode = "translucent";
    inverseTranslucentProgram.nodes[2].inputs[2].value = "0.75";
    const auto inverseTranslucent =
        hdcodex::CompileMaterialXClosure(inverseTranslucentProgram);
    Check(std::abs(inverseTranslucent.translucentWeight - 0.2F) < 1e-5F &&
              std::abs(inverseTranslucent.diffuseWeight - 1.0F) < 1e-5F,
          "inverse MaterialX diffuse/translucent mix changed lobe weights");

    const hdcodex::MaterialXGeneratedProgram combinedEdfProgram{
        .outputNode = "surface",
        .nodes = {
            {.name = "warm", .category = "uniform_edf",
             .nodeDef = "ND_uniform_edf", .type = "EDF", .inputs = {
                 {.name = "color", .type = "color3", .value = "1,0.2,0.1"}}},
            {.name = "cool", .category = "uniform_edf",
             .nodeDef = "ND_uniform_edf", .type = "EDF", .inputs = {
                 {.name = "color", .type = "color3", .value = "0.1,0.4,0.8"}}},
            {.name = "blend", .category = "mix", .nodeDef = "ND_mix_edf",
             .type = "EDF", .inputs = {
                 {.name = "fg", .type = "EDF", .upstreamNode = "warm"},
                 {.name = "bg", .type = "EDF", .upstreamNode = "cool"},
                 {.name = "mix", .type = "float", .value = "0.25"}}},
            {.name = "fill", .category = "uniform_edf",
             .nodeDef = "ND_uniform_edf", .type = "EDF", .inputs = {
                 {.name = "color", .type = "color3", .value = "0.1,0.1,0.1"}}},
            {.name = "sum", .category = "add", .nodeDef = "ND_add_edf",
             .type = "EDF", .inputs = {
                 {.name = "in1", .type = "EDF", .upstreamNode = "blend"},
                 {.name = "in2", .type = "EDF", .upstreamNode = "fill"}}},
            {.name = "scaled", .category = "multiply",
             .nodeDef = "ND_multiply_edf", .type = "EDF", .inputs = {
                 {.name = "in1", .type = "EDF", .upstreamNode = "sum"},
                 {.name = "in2", .type = "float", .value = "0.5"}}},
            {.name = "surface", .category = "surface", .nodeDef = "ND_surface",
             .type = "surfaceshader", .inputs = {
                 {.name = "edf", .type = "EDF", .upstreamNode = "scaled"}}},
        }};
    const auto combinedEdf =
        hdcodex::CompileMaterialXClosure(combinedEdfProgram);
    Check(std::abs(combinedEdf.emission[0] - 0.2125F) < 1e-5F &&
              std::abs(combinedEdf.emission[1] - 0.225F) < 1e-5F &&
              std::abs(combinedEdf.emission[2] - 0.3625F) < 1e-5F &&
              std::abs(combinedEdf.emissionWeight - 1.0F) < 1e-5F,
          "MaterialX EDF mix/add/multiply combiners changed emitted radiance");

    const hdcodex::MaterialXGeneratedProgram vectorMathProgram{
        .outputNode = "surface",
        .nodes = {
            {.name = "dot", .category = "dotproduct",
             .nodeDef = "ND_dotproduct_vector3", .type = "float", .inputs = {
                 {.name = "in1", .type = "vector3", .value = "1,2,3"},
                 {.name = "in2", .type = "vector3", .value = "4,-1,2"}}},
            {.name = "modulo", .category = "modulo",
             .nodeDef = "ND_modulo_float", .type = "float", .inputs = {
                 {.name = "in1", .type = "float", .upstreamNode = "dot"},
                 {.name = "in2", .type = "float", .value = "7"}}},
            {.name = "cross", .category = "crossproduct",
             .nodeDef = "ND_crossproduct_vector3", .type = "vector3", .inputs = {
                 {.name = "in1", .type = "vector3", .value = "1,0,0"},
                 {.name = "in2", .type = "vector3", .value = "0,1,0"}}},
            {.name = "magnitude", .category = "magnitude",
             .nodeDef = "ND_magnitude_vector3", .type = "float", .inputs = {
                 {.name = "in", .type = "vector3", .upstreamNode = "cross"}}},
            {.name = "normalized", .category = "normalize",
             .nodeDef = "ND_normalize_vector3", .type = "vector3", .inputs = {
                 {.name = "in", .type = "vector3", .value = "0,3,4"}}},
            {.name = "normalized_z", .category = "extract",
             .nodeDef = "ND_extract_vector3", .type = "float", .inputs = {
                 {.name = "in", .type = "vector3", .upstreamNode = "normalized"},
                 {.name = "index", .type = "integer", .value = "2"}}},
            {.name = "right_angle", .category = "acos",
             .nodeDef = "ND_acos_float", .type = "float", .inputs = {
                 {.name = "in", .type = "float", .value = "0"}}},
            {.name = "sine", .category = "sin",
             .nodeDef = "ND_sin_float", .type = "float", .inputs = {
                 {.name = "in", .type = "float", .upstreamNode = "right_angle"}}},
            {.name = "atan", .category = "atan2",
             .nodeDef = "ND_atan2_float", .type = "float", .inputs = {
                 {.name = "iny", .type = "float", .value = "1"},
                 {.name = "inx", .type = "float", .value = "1"}}},
            {.name = "atan_unit", .category = "divide",
             .nodeDef = "ND_divide_float", .type = "float", .inputs = {
                 {.name = "in1", .type = "float", .upstreamNode = "atan"},
                 {.name = "in2", .type = "float", .value = "0.78539816339"}}},
            {.name = "sum1", .category = "add", .nodeDef = "ND_add_float",
             .type = "float", .inputs = {
                 {.name = "in1", .type = "float", .upstreamNode = "sine"},
                 {.name = "in2", .type = "float", .upstreamNode = "normalized_z"}}},
            {.name = "sum2", .category = "add", .nodeDef = "ND_add_float",
             .type = "float", .inputs = {
                 {.name = "in1", .type = "float", .upstreamNode = "sum1"},
                 {.name = "in2", .type = "float", .upstreamNode = "atan_unit"}}},
            {.name = "exponential", .category = "exp",
             .nodeDef = "ND_exp_float", .type = "float", .inputs = {
                 {.name = "in", .type = "float", .value = "0"}}},
            {.name = "inverse_root", .category = "inversesqrt",
             .nodeDef = "ND_inversesqrt_float", .type = "float", .inputs = {
                 {.name = "in", .type = "float", .value = "4"}}},
            {.name = "sum3", .category = "add", .nodeDef = "ND_add_float",
             .type = "float", .inputs = {
                 {.name = "in1", .type = "float", .upstreamNode = "sum2"},
                 {.name = "in2", .type = "float", .upstreamNode = "exponential"}}},
            {.name = "sum4", .category = "add", .nodeDef = "ND_add_float",
             .type = "float", .inputs = {
                 {.name = "in1", .type = "float", .upstreamNode = "sum3"},
                 {.name = "in2", .type = "float", .upstreamNode = "inverse_root"}}},
            {.name = "color", .category = "combine3",
             .nodeDef = "ND_combine3_color3", .type = "color3", .inputs = {
                 {.name = "in1", .type = "float", .upstreamNode = "modulo"},
                 {.name = "in2", .type = "float", .upstreamNode = "magnitude"},
                 {.name = "in3", .type = "float", .upstreamNode = "sum4"}}},
            {.name = "emission", .category = "uniform_edf",
             .nodeDef = "ND_uniform_edf", .type = "EDF", .inputs = {
                 {.name = "color", .type = "color3", .upstreamNode = "color"}}},
            {.name = "surface", .category = "surface", .nodeDef = "ND_surface",
             .type = "surfaceshader", .inputs = {
                 {.name = "edf", .type = "EDF", .upstreamNode = "emission"}}},
        }};
    const auto vectorMath = hdcodex::CompileMaterialXClosure(vectorMathProgram);
    Check(std::abs(vectorMath.emission[0] - 1.0F) < 1e-5F &&
              std::abs(vectorMath.emission[1] - 1.0F) < 1e-5F &&
              std::abs(vectorMath.emission[2] - 4.3F) < 1e-5F,
          "MaterialX constant vector/scalar math changed graph evaluation");

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

    constexpr auto sheenXml = R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <sheen_bsdf name="sheen" type="BSDF">
    <input name="weight" type="float" value="0.7"/>
    <input name="color" type="color3" value="1, 0.2, 0.1"/>
    <input name="roughness" type="float" value="0.45"/>
    <input name="mode" type="string" value="zeltner"/>
  </sheen_bsdf>
  <surface name="surface" type="surfaceshader">
    <input name="bsdf" type="BSDF" nodename="sheen"/>
  </surface>
  <surfacematerial name="material" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="surface"/>
  </surfacematerial>
</materialx>)mtlx";
    const auto sheen = compiler.CompileXml(sheenXml, "sheen_test");
    Check(sheen.closure && std::abs(sheen.closure->sheen - 0.7F) < 1e-5F &&
              std::abs(sheen.closure->sheenColor[1] - 0.2F) < 1e-5F &&
              std::abs(sheen.closure->sheenRoughness - 0.45F) < 1e-5F &&
              sheen.closure->sheenMode == 1U,
          "MaterialX sheen primitive did not compile into the closure ABI");

    constexpr auto anisotropicXml = R"mtlx(<?xml version="1.0"?>
<materialx version="1.39">
  <conductor_bsdf name="metal" type="BSDF">
    <input name="ior" type="color3" value="0.2, 0.4, 1.2"/>
    <input name="extinction" type="color3" value="3.0, 2.4, 1.8"/>
    <input name="roughness" type="vector2" value="0.15, 0.55"/>
  </conductor_bsdf>
  <surface name="surface" type="surfaceshader">
    <input name="bsdf" type="BSDF" nodename="metal"/>
  </surface>
  <surfacematerial name="material" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="surface"/>
  </surfacematerial>
</materialx>)mtlx";
    const auto anisotropic = compiler.CompileXml(
        anisotropicXml, "anisotropic_roughness_test");
    Check(anisotropic.closure &&
              std::abs(anisotropic.closure->roughness - std::sqrt(0.15F)) < 1e-5F &&
              std::abs(anisotropic.closure->roughnessV - std::sqrt(0.55F)) < 1e-5F,
          "MaterialX anisotropic roughness lost its second axis");

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
