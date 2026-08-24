include(FetchContent)

set(FETCHCONTENT_BASE_DIR "${PROJECT_SOURCE_DIR}/_deps" CACHE PATH
    "Shared local source/build directory for fetched dependencies" FORCE)

if(NOT HDCODEX_FETCH_DEPENDENCIES)
  find_package(VulkanHeaders CONFIG QUIET)
  find_package(volk CONFIG QUIET)
  find_package(VulkanMemoryAllocator CONFIG QUIET)
  if(NOT TARGET Vulkan::Headers OR NOT TARGET volk)
    message(FATAL_ERROR
      "Vulkan development dependencies were not found. Configure with "
      "-DHDCODEX_FETCH_DEPENDENCIES=ON to download and build them under _deps.")
  endif()
else()
  set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG vulkan-sdk-1.4.341.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(vulkan_headers)

  set(VOLK_INSTALL OFF CACHE BOOL "" FORCE)
  set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.4.341.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(volk)
  target_link_libraries(volk PUBLIC Vulkan::Headers)

  set(VMA_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
  set(VMA_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(vma)
endif()
