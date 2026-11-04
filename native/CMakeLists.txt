# CMake build of the SayCheese addon for macOS (Apple Silicon). Windows builds
# with build-addon-win.bat instead; both compile the same sources.
#
# Produces, in native/build/out:
#   saycheese.uxpaddon   the addon (a dylib) the panel loads
#   saycheese-cli        the test harness (see README, Testing the engine)
# FFmpeg is linked statically; libmediapipe is loaded at runtime from the
# addon's own folder, so it isn't linked here.
#
#   bash native/build-ffmpeg-mac.sh
#   cmake -S native -B native/build/cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build native/build/cmake
# or just native/build-addon-mac.sh, which also copies the addon into plugin/.

cmake_minimum_required(VERSION 3.20)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0 CACHE STRING "")
project(saycheese_addon LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Export everything: the UXP glue macros don't annotate visibility.
set(CMAKE_C_VISIBILITY_PRESET default)
set(CMAKE_CXX_VISIBILITY_PRESET default)

set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/build/ffmpeg-out" CACHE PATH "Static FFmpeg prefix (include/, lib/)")
set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(UXP "${CMAKE_CURRENT_SOURCE_DIR}/uxp/src")
set(OUT "${CMAKE_CURRENT_SOURCE_DIR}/build/out")

foreach(lib avformat avcodec swscale avutil)
  find_library(FF_${lib} ${lib} PATHS ${FFMPEG_ROOT}/lib NO_DEFAULT_PATH REQUIRED)
endforeach()

# The engine, shared by the addon and the CLI.
add_library(saycheese_core STATIC
  ${SRC}/frame_reader.cpp
  ${SRC}/face.cpp
  ${SRC}/eye_model.cpp
  ${SRC}/jpeg_writer.cpp
  ${SRC}/job.cpp
)
target_include_directories(saycheese_core PUBLIC ${FFMPEG_ROOT}/include ${SRC})
target_compile_definitions(saycheese_core PUBLIC NDEBUG)
set_target_properties(saycheese_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(saycheese_core PUBLIC
  ${FF_avformat} ${FF_avcodec} ${FF_swscale} ${FF_avutil}
  m pthread ${CMAKE_DL_LIBS}
)

add_library(saycheese_addon SHARED
  ${SRC}/module_saycheese.cpp
  ${UXP}/utilities/UxpAddon.cpp
  ${UXP}/utilities/UxpTask.cpp
  ${UXP}/utilities/UxpValue.cpp
)
target_include_directories(saycheese_addon PRIVATE ${UXP} ${UXP}/api ${UXP}/utilities)
target_link_libraries(saycheese_addon PRIVATE saycheese_core)
set_target_properties(saycheese_addon PROPERTIES
  PREFIX ""
  OUTPUT_NAME "saycheese"
  SUFFIX ".uxpaddon"
  LIBRARY_OUTPUT_DIRECTORY ${OUT}
  MACOSX_RPATH OFF
  # Not the build machine's path. UXP loads it by path, so this is just a label.
  BUILD_WITH_INSTALL_NAME_DIR ON
  INSTALL_NAME_DIR "@loader_path"
)

add_executable(saycheese_cli ${SRC}/cli.cpp)
target_link_libraries(saycheese_cli PRIVATE saycheese_core)
set_target_properties(saycheese_cli PROPERTIES
  OUTPUT_NAME "saycheese-cli"
  RUNTIME_OUTPUT_DIRECTORY ${OUT}
)

# Ad-hoc signatures, which Premiere's hardened runtime accepts for addons.
foreach(t saycheese_addon saycheese_cli)
  add_custom_command(TARGET ${t} POST_BUILD
    COMMAND codesign --force --sign - "$<TARGET_FILE:${t}>"
    COMMENT "Ad-hoc signing $<TARGET_FILE_NAME:${t}>")
endforeach()
