add_executable(hdCodexCoreTests core_tests.cpp)
target_link_libraries(hdCodexCoreTests PRIVATE hdCodex::Core)

add_test(NAME hdCodex.Core COMMAND hdCodexCoreTests)

