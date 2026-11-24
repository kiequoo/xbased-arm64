# The PC executable (Windows, Linux): the SDL3 front end (app/win_main.c)
# on the core, with the presentation and the frame pacing of the Android
# app's (app/present.c, app/vsync.c), and the dashboard (app/ui/).
set(NC_APP_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/app/win_main.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/win_mods.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/mod_fit.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/mod_jmp.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/win_auto.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/win_devctl.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/win_aslr.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/win_frame.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/present.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/vsync.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/ui.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/i18n.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/config.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/console_cfg.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/xbox_data.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/library.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/input_map.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/pointer.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/sound.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash_settings.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/cheat_text.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash_memory.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash_browse.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash_find.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash_cover.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/xemu_import.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/update.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/shortcut.c"
)
if(WIN32)
  enable_language(RC)
  add_executable(xbased WIN32 ${NC_APP_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/app/win/xbased.rc")
  # the linker's map: the profile's offsets resolved where there is no debug info (XBASED_PROF)
  target_link_options(xbased PRIVATE "-Wl,-Map=$<TARGET_FILE_DIR:xbased>/xbased.map")
  target_link_libraries(xbased PRIVATE ole32 uuid shell32 mpr)   # the desktop shortcuts (app/ui/shortcut.c); mpr: a network drive's share (dash_browse.c)
else()
  add_executable(xbased ${NC_APP_SOURCES})
endif()
# xbased (.exe), with its icon and version on Windows (app/win/xbased.rc)
set_target_properties(xbased PROPERTIES OUTPUT_NAME xbased)
# xbased's version, set here alone: the app's (xbased_version.h, from
# app/version.h.in) and the executable's version information (xbased.rc)
set(XBASED_VERSION 1.2.11)
string(REPLACE "." "," XBASED_VERSION_RC "${XBASED_VERSION}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/app/version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/gen/xbased_version.h" @ONLY)
if(WIN32)
  # (the resource compiler leaves no list of its includes: the version's
  # header named, else xbased.rc kept an older version after a bump)
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/app/win/xbased.rc" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/gen/xbased_version.h")
endif()
target_include_directories(xbased PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen")
target_link_libraries(xbased PRIVATE xbased_native xbased_hle SDL3::SDL3)
if(XBASED_DEVCTL AND WIN32)
  target_link_libraries(xbased PRIVATE ws2_32)
endif()
target_include_directories(xbased PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../third_party/stb")  # public domain, vendored
target_compile_options(xbased PRIVATE -Wall -Wno-unused-parameter -Wno-unused-function)
# Cross-compile: win_frame.c needs X11 headers (type defs only; calls via dlopen).
# A symlink from the build dir exposes only X11/ — no host glibc alongside it, so
# Xlib.h's transitive includes (<sys/types.h> etc.) still resolve from the sysroot.
if(CMAKE_CROSSCOMPILING AND EXISTS "/usr/include/X11/Xlib.h")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/x11-host")
    file(CREATE_LINK "/usr/include/X11" "${CMAKE_BINARY_DIR}/x11-host/X11" SYMBOLIC)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/app/win_frame.c"
        PROPERTIES COMPILE_OPTIONS "-I${CMAKE_BINARY_DIR}/x11-host"
    )
endif()
# the mods and cheats the game folders hold (mods/README.md), and the
# dashboard's fonts, beside the executable
add_custom_command(TARGET xbased POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${NC_REPO}/mods" "$<TARGET_FILE_DIR:xbased>/mods"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${NC_REPO}/cheats" "$<TARGET_FILE_DIR:xbased>/cheats"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/fonts" "$<TARGET_FILE_DIR:xbased>/fonts")
