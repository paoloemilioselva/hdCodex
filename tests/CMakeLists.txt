add_executable(hdCodexCoreTests core_tests.cpp)
target_link_libraries(hdCodexCoreTests PRIVATE hdCodex::Core)

add_test(NAME hdCodex.Core COMMAND hdCodexCoreTests)

if(HDCODEX_ENABLE_VULKAN)
  add_executable(hdCodexGpuProbe gpu_probe.cpp)
  target_link_libraries(hdCodexGpuProbe PRIVATE hdCodex::Gpu)
  add_test(NAME hdCodex.GpuProbe COMMAND hdCodexGpuProbe)

  if(HDCODEX_ENABLE_SHADER_COMPILER)
    add_executable(hdCodexGlslCompilerTests glsl_compiler_tests.cpp)
    target_link_libraries(hdCodexGlslCompilerTests PRIVATE hdCodex::Gpu)
    add_test(NAME hdCodex.GlslCompiler COMMAND hdCodexGlslCompilerTests)

    add_executable(hdCodexPathTracerTests path_tracer_tests.cpp)
    target_link_libraries(hdCodexPathTracerTests PRIVATE hdCodex::Gpu)
    add_test(NAME hdCodex.PathTracer COMMAND hdCodexPathTracerTests)
  endif()
endif()

if(HDCODEX_ENABLE_MATERIALX)
  add_executable(hdCodexMaterialXCompilerTests materialx_compiler_tests.cpp)
  target_link_libraries(hdCodexMaterialXCompilerTests PRIVATE hdCodex::MaterialX)
  add_test(NAME hdCodex.MaterialXCompiler COMMAND hdCodexMaterialXCompilerTests)
  set_property(TEST hdCodex.MaterialXCompiler PROPERTY ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:${HDCODEX_OPENUSD_ROOT}/bin"
    "PATH=path_list_prepend:${HDCODEX_OPENUSD_ROOT}/lib"
  )
endif()
