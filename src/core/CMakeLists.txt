add_library(hdCodexCore STATIC
  ${PROJECT_SOURCE_DIR}/include/hdcodex/core/display_transform.h
  ${PROJECT_SOURCE_DIR}/include/hdcodex/core/shading_mode.h
  display_transform.cpp
  hash.cpp
  shader_cache.cpp
)
add_library(hdCodex::Core ALIAS hdCodexCore)

target_link_libraries(hdCodexCore PUBLIC hdCodexProjectOptions)
target_include_directories(hdCodexCore
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(hdCodexCore PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  OUTPUT_NAME hdCodexCore
)
