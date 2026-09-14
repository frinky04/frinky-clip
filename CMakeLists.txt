cmake_minimum_required(VERSION 3.28)
project(FrinkyClip VERSION 0.3.0 LANGUAGES CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Frinky Clip requires MSVC and 64-bit Windows.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(OBS_ROOT "C:/Program Files/obs-studio" CACHE PATH "OBS 32.2.2 installation, the build-time source of the runtime files")
set(DEPS "${CMAKE_SOURCE_DIR}/.deps")
set(VELOPACK_ROOT "${DEPS}/velopack-1.2.0")
configure_file(cmake/version.h.in include/version.h @ONLY)
configure_file(cmake/version.rc.in version.rc @ONLY)
add_library(velopack SHARED IMPORTED)
set_target_properties(velopack PROPERTIES
  IMPORTED_IMPLIB "${VELOPACK_ROOT}/lib/velopack_libc_win_x64_msvc.dll.lib"
  IMPORTED_LOCATION "${VELOPACK_ROOT}/lib/velopack_libc_win_x64_msvc.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${VELOPACK_ROOT}/include")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/include/libavutil")
file(WRITE "${CMAKE_BINARY_DIR}/include/obsconfig.h" "#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n#define OBS_DATA_PATH \"data\"\n#define OBS_INSTALL_PREFIX \"\"\n")
file(WRITE "${CMAKE_BINARY_DIR}/include/libavutil/avconfig.h" "#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n#endif\n")
get_filename_component(MSVC_BIN "${CMAKE_LINKER}" DIRECTORY)
# The installed runtime has no SDK import libraries. Generate them from its
# export tables; public headers are pinned to the matching OBS / FFmpeg ABI.
function(import_runtime target dll)
  execute_process(COMMAND "${MSVC_BIN}/dumpbin.exe" /exports "${OBS_ROOT}/bin/64bit/${dll}.dll"
    OUTPUT_VARIABLE exports COMMAND_ERROR_IS_FATAL ANY)
  string(REGEX MATCHALL "[ \t]+[0-9]+[ \t]+[0-9A-F]+[ \t]+[0-9A-F]+[ \t]+[^\r\n]+" rows "${exports}")
  set(def "LIBRARY ${dll}.dll\nEXPORTS\n")
  foreach(row IN LISTS rows)
    string(REGEX REPLACE "^[ \t]+[0-9]+[ \t]+[0-9A-F]+[ \t]+[0-9A-F]+[ \t]+([^ \r\n]+).*" "\\1" symbol "${row}")
    string(APPEND def "${symbol}\n")
  endforeach()
  file(WRITE "${CMAKE_BINARY_DIR}/${target}.def" "${def}")
  execute_process(COMMAND "${MSVC_BIN}/lib.exe" /nologo /machine:x64 "/def:${CMAKE_BINARY_DIR}/${target}.def"
    "/out:${CMAKE_BINARY_DIR}/${target}.lib" OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
  add_library(${target} SHARED IMPORTED)
  set_target_properties(${target} PROPERTIES IMPORTED_IMPLIB "${CMAKE_BINARY_DIR}/${target}.lib"
    IMPORTED_LOCATION "${OBS_ROOT}/bin/64bit/${dll}.dll")
endfunction()
import_runtime(obs obs)
import_runtime(avformat avformat-62)
import_runtime(avcodec avcodec-62)
import_runtime(avutil avutil-60)
import_runtime(swscale swscale-9)
import_runtime(swresample swresample-6)
add_library(clip_core src/common.cpp src/media.cpp src/buffer.cpp src/timeline.cpp src/export.cpp)
target_include_directories(clip_core PUBLIC src "${CMAKE_BINARY_DIR}/include")
target_include_directories(clip_core SYSTEM PUBLIC "${DEPS}/obs-src/libobs" "${DEPS}/ffmpeg-src")
target_compile_definitions(clip_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(clip_core PUBLIC obs avformat avcodec avutil swscale swresample shell32 ole32)
add_executable(frinky-clip WIN32 src/main.cpp src/recorder.cpp src/ui.cpp src/app.cpp src/updates.cpp src/thumbs.cpp src/waveform.cpp src/player.cpp "${CMAKE_BINARY_DIR}/version.rc"
  "${DEPS}/imgui/imgui.cpp" "${DEPS}/imgui/imgui_draw.cpp" "${DEPS}/imgui/imgui_tables.cpp" "${DEPS}/imgui/imgui_widgets.cpp"
  "${DEPS}/imgui/backends/imgui_impl_win32.cpp" "${DEPS}/imgui/backends/imgui_impl_dx11.cpp")
target_include_directories(frinky-clip PRIVATE "${DEPS}/imgui" "${DEPS}/imgui/backends")
target_link_libraries(frinky-clip PRIVATE clip_core velopack d3d11 dxgi d3dcompiler dwmapi comctl32 dbghelp)
target_compile_options(frinky-clip PRIVATE /Zi)
target_link_options(frinky-clip PRIVATE /DEBUG)
target_compile_options(clip_core PRIVATE /W4 /utf-8 /EHsc)
target_compile_options(frinky-clip PRIVATE /W4 /utf-8 /EHsc)
add_custom_command(TARGET frinky-clip POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:velopack>" "$<TARGET_FILE_DIR:frinky-clip>/velopack_libc.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/NotoSansMono-Medium.ttf" "$<TARGET_FILE_DIR:frinky-clip>/NotoSansMono-Medium.ttf")
enable_testing()
add_executable(clip-tests tests/core_tests.cpp src/waveform.cpp)
target_link_libraries(clip-tests PRIVATE clip_core)
add_test(NAME clip-core COMMAND clip-tests)
# Opt-in real editor benchmark. Its test-only App never owns a recorder or tray.
add_executable(editor-bench EXCLUDE_FROM_ALL WIN32 tests/editor_bench.cpp src/ui.cpp src/updates.cpp src/player.cpp src/thumbs.cpp src/waveform.cpp
  "${DEPS}/imgui/imgui.cpp" "${DEPS}/imgui/imgui_draw.cpp" "${DEPS}/imgui/imgui_tables.cpp" "${DEPS}/imgui/imgui_widgets.cpp"
  "${DEPS}/imgui/backends/imgui_impl_win32.cpp" "${DEPS}/imgui/backends/imgui_impl_dx11.cpp")
target_include_directories(editor-bench PRIVATE "${DEPS}/imgui" "${DEPS}/imgui/backends")
target_compile_options(editor-bench PRIVATE /W4 /utf-8 /EHsc)
target_link_libraries(editor-bench PRIVATE clip_core velopack d3d11 dxgi d3dcompiler dwmapi comctl32)
add_custom_command(TARGET editor-bench POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:velopack>" "$<TARGET_FILE_DIR:editor-bench>/velopack_libc.dll")
# Release layout: everything the app loads at runtime is staged beside the
# executable (see cmake/stage.cmake), so a copy of the output folder runs on a
# machine with only the NVIDIA driver. The OBS installation is a build input.
set(OBS_RUNTIME_DLLS obs libobs-d3d11 libobs-winrt avcodec-62 avdevice-62 avfilter-11 avformat-62 avutil-60 swresample-6 swscale-9 libx264-164 libcurl librist srt w32-pthreads zlib)
set(OBS_RUNTIME_EXES obs-ffmpeg-mux obs-nvenc-test obs-amf-test)
set(OBS_PLUGINS obs-ffmpeg obs-nvenc win-capture win-wasapi image-source)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
# One staging step after both executables: they share an output folder, and
# concurrent copies into it collide.
add_custom_target(stage ALL
  COMMAND ${CMAKE_COMMAND} "-DOBS_ROOT=${OBS_ROOT}" "-DDEST=$<TARGET_FILE_DIR:frinky-clip>" "-DDLLS=${OBS_RUNTIME_DLLS}" "-DEXES=${OBS_RUNTIME_EXES}"
    "-DPLUGINS=${OBS_PLUGINS}" "-DCRT=${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}" -P "${CMAKE_SOURCE_DIR}/cmake/stage.cmake"
  DEPENDS frinky-clip clip-tests COMMENT "Staging runtime files beside the executables")
