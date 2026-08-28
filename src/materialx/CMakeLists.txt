add_library(hdCodexMaterialX STATIC
  materialx_closure.cpp
  materialx_compiler.cpp
)
add_library(hdCodex::MaterialX ALIAS hdCodexMaterialX)

target_link_libraries(hdCodexMaterialX
  PUBLIC
    hdCodexProjectOptions
    hdCodex::Core
    hdCodex::Gpu
    MaterialXCore
  PRIVATE
    hdMtlx
    MaterialXFormat
    MaterialXGenShader
    MaterialXGenGlsl
)
target_include_directories(hdCodexMaterialX
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_definitions(hdCodexMaterialX PRIVATE
  HDCODEX_MATERIALX_DATA_ROOT="${HDCODEX_OPENUSD_ROOT}"
)
set_target_properties(hdCodexMaterialX PROPERTIES POSITION_INDEPENDENT_CODE ON)
