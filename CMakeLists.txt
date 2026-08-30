cmake_minimum_required(VERSION 3.25)

project(hdCodex VERSION 0.1.0 LANGUAGES C CXX)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(GNUInstallDirs)

option(HDCODEX_BUILD_HYDRA_PLUGIN "Build the OpenUSD Hydra render delegate" ON)
option(HDCODEX_ENABLE_VULKAN "Build the Vulkan hardware ray-tracing backend" ON)
option(HDCODEX_ENABLE_MATERIALX "Build runtime MaterialX shader generation" ON)
option(HDCODEX_ENABLE_SHADER_COMPILER "Build the in-process GLSL to SPIR-V compiler" ON)
option(HDCODEX_FETCH_DEPENDENCIES "Fetch source-buildable dependencies into the build tree" OFF)
option(HDCODEX_WARNINGS_AS_ERRORS "Treat hdCodex compiler warnings as errors" OFF)

set(HDCODEX_OPENUSD_ROOT "C:/dev/usd-26.03" CACHE PATH "OpenUSD install root")
set(HDCODEX_SHADER_CACHE_DIR "shader-cache" CACHE STRING "Default runtime shader cache directory")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX
      "C:/Users/paolo/Desktop/usd-26.03-extra"
      CACHE PATH "hdCodex installation prefix" FORCE)
endif()

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

if(HDCODEX_ENABLE_VULKAN)
  include(cmake/Dependencies.cmake)
  add_subdirectory(src/gpu)
endif()

if(HDCODEX_BUILD_HYDRA_PLUGIN OR HDCODEX_ENABLE_MATERIALX)
  include(cmake/OpenUSD.cmake)
endif()

if(HDCODEX_ENABLE_MATERIALX)
  if(NOT HDCODEX_ENABLE_VULKAN OR NOT HDCODEX_ENABLE_SHADER_COMPILER)
    message(FATAL_ERROR "MaterialX compilation requires the Vulkan shader compiler")
  endif()
  add_subdirectory(src/materialx)
endif()

if(HDCODEX_BUILD_HYDRA_PLUGIN)
  add_subdirectory(src/hydra)
endif()

if(HDCODEX_BUILD_HYDRA_PLUGIN OR HDCODEX_ENABLE_MATERIALX)
  add_subdirectory(tools)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
