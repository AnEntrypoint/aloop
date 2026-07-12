# aloop native binary — the ported loop engine + in-process LV2 host + Link +
# control, built as one process with per-core RT threads.
#
# WHY the structure: the DSP core comes from ../looper unchanged (it's Circle-free
# and allocation-free, ADR / MIGRATION-MAP); the host/link/control/net layers are
# aloop's Linux replacements for the bare-metal subsystems. Built for aarch64
# under musl in CI (.github/workflows/build-binary.yml).
cmake_minimum_required(VERSION 3.16)
project(aloop CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-O2 -Wall)

# Where the ported looper DSP lives. Default to the sibling checkout; CI can
# override with -DLOOPER_DIR=<vendored snapshot> for a pinned, reproducible build.
set(LOOPER_DIR "${CMAKE_SOURCE_DIR}/../../looper" CACHE PATH "path to the looper DSP source")

# The DSP translation units ported unchanged (see src/dsp/PORTED-FILES.md).
# Header-only effects (apcEffectsProcessor.h, microRepeat.h, soladSnacOctaver.h,
# grainFormant.h, paramSnapshot.h) are pulled via include paths.
set(DSP_SOURCES
  # ${LOOPER_DIR}/loopMachine.cpp        # wired when the vendored snapshot is pinned
  dsp/audio_thread.cpp
  host/lv2_host.cpp
  link/link_bridge.cpp
  control/telemetry.cpp
  main.cpp
)

# Dependencies (musl-built in the Alpine container): ALSA (audio I/O), lilv
# (LV2 discovery), the LV2 headers. Ableton Link is header-only (vendored).
find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(LILV lilv-0)

add_executable(aloop ${DSP_SOURCES})
target_include_directories(aloop PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${LOOPER_DIR}
  ${LOOPER_DIR}/patches
  ${ALSA_INCLUDE_DIRS}
  ${LILV_INCLUDE_DIRS}
  third_party/link/include   # vendored Ableton Link headers
)
target_link_libraries(aloop PRIVATE ${ALSA_LIBRARIES} ${LILV_LIBRARIES} pthread dl)

# NOTE: the .cpp implementations (audio_thread.cpp, lv2_host.cpp, link_bridge.cpp,
# telemetry.cpp, main.cpp) are filled against the headers in this tree as their
# PRD rows drain. The headers already define the full interface + RT contract, so
# this CMake compiles the moment each .cpp lands — the build graph is complete.
