cmake_minimum_required(VERSION 3.25)

project(hdCodex VERSION 0.1.0 LANGUAGES CXX)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(GNUInstallDirs)

option(HDCODEX_BUILD_HYDRA_PLUGIN "Build the OpenUSD Hydra render delegate" ON)
option(HDCODEX_ENABLE_VULKAN "Build the Vulkan hardware ray-tracing backend" ON)
option(HDCODEX_ENABLE_MATERIALX "Build runtime MaterialX shader generation" ON)
option(HDCODEX_FETCH_DEPENDENCIES "Fetch source-buildable dependencies into the build tree" OFF)
option(HDCODEX_WARNINGS_AS_ERRORS "Treat hdCodex compiler warnings as errors" OFF)

set(HDCODEX_OPENUSD_ROOT "" CACHE PATH "OpenUSD install root (or a Houdini install root)")
set(HDCODEX_SHADER_CACHE_DIR "shader-cache" CACHE STRING "Default runtime shader cache directory")

add_library(hdCodexProjectOptions INTERFACE)
target_compile_features(hdCodexProjectOptions INTERFACE cxx_std_20)
if(MSVC)
  target_compile_options(hdCodexProjectOptions INTERFACE /W4 /permissive- /Zc:__cplusplus)
  if(HDCODEX_WARNINGS_AS_ERRORS)
    target_compile_options(hdCodexProjectOptions INTERFACE /WX)
  endif()
else()
  target_compile_options(hdCodexProjectOptions INTERFACE -Wall -Wextra -Wpedantic)
  if(HDCODEX_WARNINGS_AS_ERRORS)
    target_compile_options(hdCodexProjectOptions INTERFACE -Werror)
  endif()
endif()

add_subdirectory(src/core)

if(HDCODEX_BUILD_HYDRA_PLUGIN)
  add_subdirectory(src/hydra)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

