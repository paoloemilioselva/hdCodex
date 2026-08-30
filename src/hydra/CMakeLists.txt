add_library(hdCodexGeometry STATIC
  subdivision.cpp
)
target_link_libraries(hdCodexGeometry
  PUBLIC
    hdCodexProjectOptions
    hd
    pxOsd
)
target_include_directories(hdCodexGeometry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(hdCodexGeometry PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(hdCodex SHARED
  camera.cpp
  instancer.cpp
  light.cpp
  material.cpp
  mesh.cpp
  render_buffer.cpp
  render_delegate.cpp
  render_pass.cpp
  renderer_plugin.cpp
  texture_loader.cpp
)

target_link_libraries(hdCodex
  PRIVATE
    hdCodexGeometry
    hdCodex::Core
    $<$<BOOL:${HDCODEX_ENABLE_VULKAN}>:hdCodex::Gpu>
    $<$<BOOL:${HDCODEX_ENABLE_MATERIALX}>:hdCodex::MaterialX>
    hd
    hdx
    usdImaging
    usdShade
    usdLux
    hio
)
target_include_directories(hdCodex PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(hdCodex
  PRIVATE
    HDCODEX_EXPORTS
    NOMINMAX
    HDCODEX_SHADER_CACHE_DIR="${HDCODEX_SHADER_CACHE_DIR}"
    $<$<BOOL:${HDCODEX_ENABLE_VULKAN}>:HDCODEX_HAS_VULKAN>
    $<$<BOOL:${HDCODEX_ENABLE_MATERIALX}>:HDCODEX_HAS_MATERIALX>
)
set_target_properties(hdCodex PROPERTIES
  OUTPUT_NAME hdCodex
  PREFIX ""
  WINDOWS_EXPORT_ALL_SYMBOLS OFF
)

install(TARGETS hdCodex
  RUNTIME DESTINATION plugin/usd
  LIBRARY DESTINATION plugin/usd
)
install(FILES "${PROJECT_SOURCE_DIR}/resources/plugInfo.json"
  DESTINATION plugin/usd/hdCodex/resources
)
