# aloop native binary — a PURE LINUX project: the loop engine + in-process LV2
# host + Link + control, built as one process with per-core RT threads.
#
# WHY the structure: aloop is a Linux application. Its EFFECTS are the dubfx LV2
# plugin (a Faust-built .lv2 bundle), loaded at runtime from /effects/home — NOT
# compiled in, and NOT sourced from the Circle looper project (that is a different
# platform; no looper source appears here). The loop engine + host/link/control/
# net are aloop's own Linux code. Built for aarch64 under musl in CI.
cmake_minimum_required(VERSION 3.16)
project(aloop CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-O2 -Wall)

# The aloop source — all Linux-native, no Circle/looper dependency. The effects
# are the dubfx LV2 bundle, loaded at runtime by the in-process host (host/).
# The loop engine is FAUST (dsp/loop.dsp), compiled to C++ by the build (faust
# -lang cpp -> build/loop.cpp) and included by the audio thread — NOT a
# hand-written .cpp. The effects are the dubfx LV2, loaded at runtime by the host.
# So the DSP is Faust; these .cpp are only the thin native RT shell.
set(DSP_SOURCES
  dsp/audio_thread.cpp    # RT audio thread: drives the Faust loop engine + effect LV2 per block
  host/lv2_host.cpp       # in-process LV2 host (loads the dubfx effect bundle + user LV2)
  link/link_bridge.cpp    # Ableton Link -> the lock-free snapshot
  control/telemetry.cpp   # the diagnosability surface
  control/midi.cpp        # ALSA rawmidi + the control mapping
  main.cpp                # the wiring
)

# Compile the Faust loop engine to C++ at build time (the generated build/loop.cpp
# is #included by audio_thread.cpp). Requires the faust compiler in the build env.
add_custom_command(
  OUTPUT  ${CMAKE_BINARY_DIR}/loop.cpp
  COMMAND faust -lang cpp -cn AloopLoopDsp ${CMAKE_SOURCE_DIR}/../dsp/loop.dsp -o ${CMAKE_BINARY_DIR}/loop.cpp
  DEPENDS ${CMAKE_SOURCE_DIR}/../dsp/loop.dsp
  COMMENT "Compiling the Faust loop engine (dsp/loop.dsp) to C++"
)
add_custom_target(faust_loop DEPENDS ${CMAKE_BINARY_DIR}/loop.cpp)

# Dependencies (musl-built in the Alpine container): ALSA (audio I/O), lilv
# (LV2 discovery/loading), the LV2 headers. Ableton Link is header-only (vendored).
find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(LILV lilv-0)

add_executable(aloop ${DSP_SOURCES})
target_include_directories(aloop PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${ALSA_INCLUDE_DIRS}
  ${LILV_INCLUDE_DIRS}
  third_party/link/include   # vendored Ableton Link headers (a Linux library)
)
target_link_libraries(aloop PRIVATE ${ALSA_LIBRARIES} ${LILV_LIBRARIES} pthread dl)
add_dependencies(aloop faust_loop)
target_include_directories(aloop PRIVATE ${CMAKE_BINARY_DIR})   # for the generated loop.cpp

# The effects are NOT a build dependency — they are the dubfx LV2 bundle deployed
# to /effects/home at image-assembly time and dlopen'd at runtime. aloop the
# binary never links effect code; it hosts the plugin. That is the whole point:
# effects are a swappable file, not compiled in.
