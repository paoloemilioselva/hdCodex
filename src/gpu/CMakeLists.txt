add_library(hdCodexGpu STATIC
  spirv_reflection.cpp
  vulkan_path_tracer.cpp
  vulkan_context.cpp
)
add_library(hdCodex::Gpu ALIAS hdCodexGpu)

target_link_libraries(hdCodexGpu
  PUBLIC hdCodexProjectOptions hdCodex::Core
  PRIVATE volk Vulkan::Headers
)
if(HDCODEX_ENABLE_SHADER_COMPILER)
  target_sources(hdCodexGpu PRIVATE glsl_compiler.cpp)
  target_link_libraries(hdCodexGpu PRIVATE
    glslang::glslang
    glslang::glslang-default-resource-limits
  )
  target_compile_definitions(hdCodexGpu PUBLIC HDCODEX_HAS_SHADER_COMPILER=1)
endif()
target_include_directories(hdCodexGpu
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_definitions(hdCodexGpu PRIVATE VK_NO_PROTOTYPES)
set_target_properties(hdCodexGpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
