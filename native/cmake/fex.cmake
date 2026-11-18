# FEXCore from the FEX fork (branch hakuX), for the native core and its tests.
#
# Provides the target nc_fex: FEXCore plus FEX's host feature detection
# (taken from its front-end Common library without the rest of it), with the
# directory-scoped definitions FEX sources expect.

set(XBASED_FEX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../external/fex" CACHE PATH "Checkout of the FEX fork (branch hakuX)")
if(NOT EXISTS "${XBASED_FEX_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "No FEX fork at ${XBASED_FEX_DIR}: git submodule update --init external/fex "
    "(and its submodules, android/README.md), or set XBASED_FEX_DIR")
endif()

# FEXCore only. No thunks, tests, FEXConfig, telemetry, glibc jemalloc,
# vixl or LTO; FEX's own allocator (rpmalloc) stays on.
set(ENABLE_FEX_ALLOCATOR        ON      CACHE BOOL   "" FORCE)
set(ENABLE_JEMALLOC_GLIBC_ALLOC OFF     CACHE BOOL   "" FORCE)
set(ENABLE_OFFLINE_TELEMETRY    OFF     CACHE BOOL   "" FORCE)
set(BUILD_FEXCONFIG             OFF     CACHE BOOL   "" FORCE)
set(ENABLE_GDB_SYMBOLS          OFF     CACHE BOOL   "" FORCE)
set(ENABLE_LTO                  OFF     CACHE BOOL   "" FORCE)
set(ENABLE_CCACHE               OFF     CACHE BOOL   "" FORCE)
set(BUILD_THUNKS                OFF     CACHE BOOL   "" FORCE)
set(BUILD_TESTING               OFF     CACHE BOOL   "" FORCE)
set(ENABLE_VIXL_DISASSEMBLER    OFF     CACHE BOOL   "" FORCE)
set(ENABLE_VIXL_SIMULATOR       OFF     CACHE BOOL   "" FORCE)
set(BUILD_FEX_LINUX_TESTS       OFF     CACHE BOOL   "" FORCE)
set(ENABLE_ASSERTIONS           OFF     CACHE BOOL   "" FORCE)
set(TUNE_CPU                    none    CACHE STRING "" FORCE)

# XBASED_ARCH: the CPUs the build may assume, for FEX (its TUNE_ARCH) and the
# core alike, e.g. armv8.2-a+fp16+dotprod+rcpc (the Snapdragon 865 up: the
# RP5, the Odin 3). Empty: FEX's generic armv8-a+crc, the core's NDK default,
# which runs anywhere but calls out for every atomic read-modify-write.
set(XBASED_ARCH "" CACHE STRING "-march for FEX and the core (empty: generic)")
if(XBASED_ARCH)
  set(TUNE_ARCH "${XBASED_ARCH}" CACHE STRING "" FORCE)
  add_compile_options("-march=${XBASED_ARCH}")
else()
  set(TUNE_ARCH generic CACHE STRING "" FORCE)
endif()

if(NOT TARGET FEXCore)
  add_subdirectory("${XBASED_FEX_DIR}" "${CMAKE_BINARY_DIR}/fex-build" EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET nc_fex)
  add_library(nc_fex STATIC
    "${XBASED_FEX_DIR}/Source/Common/HostFeatures.cpp"
    "${XBASED_FEX_DIR}/Source/Common/CPUInfo.cpp"
    "${XBASED_FEX_DIR}/Source/Common/Linux/LinuxVersion.cpp"
  )
  target_compile_features(nc_fex PUBLIC cxx_std_20)
  target_include_directories(nc_fex PUBLIC
    "${XBASED_FEX_DIR}/Source"
    "${CMAKE_BINARY_DIR}/fex-build/Source"
  )
  # FEX sets these per directory (add_compile_definitions), so sources built
  # outside its tree need them repeated; the NDK's clang has preserve_all.
  target_compile_definitions(nc_fex PUBLIC
    ARCHITECTURE_arm64=1 _LARGEFILE64_SOURCE
    "FEX_PRESERVE_ALL_ATTR=__attribute__((preserve_all))" FEX_HAS_PRESERVE_ALL_ATTR=1
  )
  target_link_libraries(nc_fex PUBLIC FEXCore FEXCore_Base JemallocLibs FEXHeaderUtils range-v3::range-v3)
endif()
