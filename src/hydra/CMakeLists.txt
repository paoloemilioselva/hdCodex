list(PREPEND CMAKE_PREFIX_PATH "${HDCODEX_OPENUSD_ROOT}")
set(OpenSubdiv_DIR "${HDCODEX_OPENUSD_ROOT}/lib/cmake/OpenSubdiv" CACHE PATH "" FORCE)
set(MaterialX_DIR "${HDCODEX_OPENUSD_ROOT}/lib/cmake/MaterialX" CACHE PATH "" FORCE)
set(Imath_DIR "${HDCODEX_OPENUSD_ROOT}/lib/cmake/Imath" CACHE PATH "" FORCE)

find_package(OpenGL REQUIRED)
find_package(pxr CONFIG REQUIRED
  PATHS "${HDCODEX_OPENUSD_ROOT}"
  NO_DEFAULT_PATH
)

add_library(hdCodex SHARED
  camera.cpp
  material.cpp
  mesh.cpp
  render_buffer.cpp
  render_delegate.cpp
  render_pass.cpp
  renderer_plugin.cpp
)

target_link_libraries(hdCodex
  PRIVATE
    hdCodex::Core
    $<$<BOOL:${HDCODEX_ENABLE_VULKAN}>:hdCodex::Gpu>
    hd
    hdx
    usdImaging
    usdShade
)
target_include_directories(hdCodex PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(hdCodex
  PRIVATE
    HDCODEX_EXPORTS
    NOMINMAX
    HDCODEX_SHADER_CACHE_DIR="${HDCODEX_SHADER_CACHE_DIR}"
    $<$<BOOL:${HDCODEX_ENABLE_VULKAN}>:HDCODEX_HAS_VULKAN>
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
