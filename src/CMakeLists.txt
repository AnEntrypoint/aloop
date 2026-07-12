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

# The Faust loop engine is compiled to C++ (loop.cpp) and #included by
# audio_thread.cpp. CI pre-generates it (where faust exists) into
# ${CMAKE_BINARY_DIR}/loop.cpp; if it is already there we use it, otherwise we
# generate it here (local builds that have faust). Either way loop.cpp ends up in
# the binary dir on the include path.
if(NOT EXISTS ${CMAKE_BINARY_DIR}/loop.cpp)
  find_program(FAUST_BIN faust)
  if(FAUST_BIN)
    add_custom_command(
      OUTPUT  ${CMAKE_BINARY_DIR}/loop.cpp
      COMMAND ${FAUST_BIN} -lang cpp -cn AloopLoopDsp -I ${CMAKE_SOURCE_DIR}/../dsp -I ${CMAKE_SOURCE_DIR}/../effects/home/faust ${CMAKE_SOURCE_DIR}/../dsp/aloop.dsp -o ${CMAKE_BINARY_DIR}/loop.cpp
      DEPENDS ${CMAKE_SOURCE_DIR}/../dsp/aloop.dsp ${CMAKE_SOURCE_DIR}/../dsp/loop.dsp ${CMAKE_SOURCE_DIR}/../dsp/effects_runtime.dsp
      COMMENT "Compiling the Faust home stack (dsp/aloop.dsp = loop + runtime effects) to C++")
    add_custom_target(faust_loop DEPENDS ${CMAKE_BINARY_DIR}/loop.cpp)
  else()
    message(FATAL_ERROR "loop.cpp not pre-generated and faust not found — pre-generate it or install faust")
  endif()
else()
  add_custom_target(faust_loop)   # pre-generated; nothing to do
endif()

# Dependencies (musl-built in the Alpine container): ALSA (audio I/O), lilv
# (LV2 discovery/loading), the LV2 headers.
find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(LILV lilv-0)

# Ableton Link — the official header-only C++ library. Fetched at configure time
# (CI has network); link_bridge.cpp is guarded with __has_include, so if Link is
# absent the binary still builds (Link inactive) — but CI fetches it so it is real.
include(FetchContent)
# Link bundles asio as a submodule (its platform code needs it), so fetch
# submodules. Not shallow — submodule fetch is more reliable with full history.
FetchContent_Declare(abletonlink
  GIT_REPOSITORY https://github.com/Ableton/link.git
  GIT_TAG        Link-3.1.3
  GIT_SUBMODULES_RECURSE TRUE)
set(LINK_INCLUDE "")
FetchContent_GetProperties(abletonlink)
if(NOT abletonlink_POPULATED)
  # Best-effort: don't fail the build if the fetch is unavailable (guarded source).
  FetchContent_Populate(abletonlink)
endif()
if(abletonlink_SOURCE_DIR)
  set(LINK_INCLUDE ${abletonlink_SOURCE_DIR}/include ${abletonlink_SOURCE_DIR}/modules/asio-standalone/asio/include)
endif()

add_executable(aloop ${DSP_SOURCES})
target_include_directories(aloop PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}        # the generated Faust loop.cpp
  ${CMAKE_SOURCE_DIR}/../effects/home/faust   # pitch ffunction engine headers
  ${ALSA_INCLUDE_DIRS}
  ${LILV_INCLUDE_DIRS}
  ${LINK_INCLUDE}            # Ableton Link + its bundled asio (fetched)
)
# Ableton Link needs its platform selected (Linux) and asio in standalone,
# header-only mode. These are the standard Link-on-Linux build defines.
if(LINK_INCLUDE)
  target_compile_definitions(aloop PRIVATE
    LINK_PLATFORM_LINUX=1
    ASIO_STANDALONE=1)
  # Force-include the musl compat shim (provides htonll/ntohll, which musl lacks)
  # before anything else, so Link's byte-stream serializer compiles under musl.
  target_compile_options(aloop PRIVATE -include ${CMAKE_SOURCE_DIR}/compat/musl_compat.h)
endif()
target_link_libraries(aloop PRIVATE ${ALSA_LIBRARIES} ${LILV_LIBRARIES} pthread dl)
add_dependencies(aloop faust_loop)

# The effects are NOT a build dependency — they are the dubfx LV2 bundle deployed
# to /effects/home at image-assembly time and dlopen'd at runtime. aloop the
# binary never links effect code; it hosts the plugin. That is the whole point:
# effects are a swappable file, not compiled in.
