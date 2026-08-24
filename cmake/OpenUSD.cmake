list(PREPEND CMAKE_PREFIX_PATH "${HDCODEX_OPENUSD_ROOT}")

# Dependency hints are deliberately rooted in the standalone OpenUSD install.
# Do not add Houdini search paths to this file; a Houdini ABI build must use a
# separate preset and dependency graph.
set(OpenSubdiv_DIR "${HDCODEX_OPENUSD_ROOT}/lib/cmake/OpenSubdiv" CACHE PATH "" FORCE)
set(MaterialX_DIR "${HDCODEX_OPENUSD_ROOT}/lib/cmake/MaterialX" CACHE PATH "" FORCE)
set(Imath_DIR "${HDCODEX_OPENUSD_ROOT}/lib/cmake/Imath" CACHE PATH "" FORCE)

find_package(OpenGL REQUIRED)
find_package(pxr CONFIG REQUIRED
  PATHS "${HDCODEX_OPENUSD_ROOT}"
  NO_DEFAULT_PATH
)

if(HDCODEX_ENABLE_MATERIALX)
  find_package(MaterialX CONFIG REQUIRED
    PATHS "${HDCODEX_OPENUSD_ROOT}/lib/cmake/MaterialX"
    NO_DEFAULT_PATH
  )
endif()
