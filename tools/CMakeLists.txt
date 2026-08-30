add_executable(hdCodexDisplayTransform display_transform.cpp)
target_link_libraries(hdCodexDisplayTransform PRIVATE hdCodex::Core hio)

install(TARGETS hdCodexDisplayTransform
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
