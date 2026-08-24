add_executable(hdCodexCoreTests core_tests.cpp)
target_link_libraries(hdCodexCoreTests PRIVATE hdCodex::Core)

add_test(NAME hdCodex.Core COMMAND hdCodexCoreTests)

if(HDCODEX_ENABLE_VULKAN)
  add_executable(hdCodexGpuProbe gpu_probe.cpp)
  target_link_libraries(hdCodexGpuProbe PRIVATE hdCodex::Gpu)
  add_test(NAME hdCodex.GpuProbe COMMAND hdCodexGpuProbe)
endif()
