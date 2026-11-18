# xbased native core on Windows x64 (MinGW-w64 clang, MSYS2 CLANG64).
#
# No FEX: the guest CPU is cpu/cpu_x86.cpp, which translates the title's
# x86-32 code to x86-64 with Zydis (its decoder and encoder), from
# NC_DEPS/zydis (v4.1.1). xxhash comes from NC_DEPS/xxhash (v0.8.3), the
# header FEX's copy is on Android. SDL3 is added here, before the APU and the
# front end link it.
set(NC_DEPS "${CMAKE_CURRENT_LIST_DIR}/../../../deps" CACHE PATH "native core third-party sources (tools/fetch_deps.sh)")

set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYCORE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
add_subdirectory("${NC_DEPS}/zydis" zydis-build EXCLUDE_FROM_ALL)
add_library(nc_zydis INTERFACE)
target_link_libraries(nc_zydis INTERFACE Zydis)

set(NC_XXHASH_DIR "${NC_DEPS}/xxhash")

# The guest's 4 GiB live at host address 0 (core/vm_win.c): the process must
# keep its own allocations above 4 GiB (high-entropy ASLR), and translated
# code adds no base to guest addresses. Pointer arithmetic on that null base
# is the core's everyday business: no optimization may assume otherwise.
add_compile_options(-fno-delete-null-pointer-checks -msse4.1 -mpopcnt -g1)
add_compile_definitions(_GNU_SOURCE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 NC_DESKTOP=1)
add_link_options(-Wl,--high-entropy-va -Wl,--dynamicbase -static)
# POSIX headers the core includes that MinGW doesn't have (compat/win), for
# the core's own targets only (nc_win_compat, at the end of CMakeLists.txt):
# third-party code has its own Windows support.
set(NC_WIN_COMPAT_INC "${CMAKE_CURRENT_LIST_DIR}/../compat/win/include")
function(nc_win_compat)
  foreach(t ${ARGN})
    target_include_directories(${t} BEFORE PRIVATE "${NC_WIN_COMPAT_INC}")
    target_compile_options(${t} PRIVATE "SHELL:-include nc_win_posix.h")
  endforeach()
endfunction()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${NC_DEPS}/SDL3" sdl3 EXCLUDE_FROM_ALL)
