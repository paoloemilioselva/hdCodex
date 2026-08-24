#include "hdcodex/core/shader_cache.h"
#include "hdcodex/materialx/materialx_compiler.h"

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

    const auto second = compiler.CompileXml(xml, "standard_surface_test");
    Check(second.vertexSpirv.cacheHit, "MaterialX vertex shader cache miss");
    Check(second.pixelSpirv.cacheHit, "MaterialX pixel shader cache miss");
    Check(second.vertexSpirv.words == first.vertexSpirv.words, "cached vertex SPIR-V changed");
    Check(second.pixelSpirv.words == first.pixelSpirv.words, "cached pixel SPIR-V changed");

    std::filesystem::remove_all(cacheRoot, error);
    std::cout << hdcodex::MaterialXCompiler::GeneratorVersion()
              << ": generation, compilation, and cache tests passed\n";
    return 0;
} catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
}
