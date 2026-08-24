add_library(hdCodexGpu STATIC
  vulkan_context.cpp
)
add_library(hdCodex::Gpu ALIAS hdCodexGpu)

target_link_libraries(hdCodexGpu
  PUBLIC hdCodexProjectOptions
  PRIVATE volk Vulkan::Headers
)
target_include_directories(hdCodexGpu
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_definitions(hdCodexGpu PRIVATE VK_NO_PROTOTYPES)
set_target_properties(hdCodexGpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
