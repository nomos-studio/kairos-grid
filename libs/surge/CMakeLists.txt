# SPDX-License-Identifier: GPL-3.0-or-later
#
# surge — Surge XT DSP core (oscillators, filters, effects, envelopes)
#
# Wraps surge's own CMake build for embedding in kairos-grid.
# Skip flags mirror surge-rack; SURGE_COMPILE_BLOCK_SIZE=32 (not 8 like VCV).
#
# Set SURGE_SRC_DIR to a local surge checkout with submodules initialised
# to avoid a network fetch (strongly recommended; the repo + submodules is large).
# If unset, FetchContent downloads commit 304f6dc with recursive submodules.
#
# The resulting aggregate target is kairos_grid::surge.

cmake_minimum_required(VERSION 3.20)
project(kairos-surge LANGUAGES C CXX)

# libsamplerate ships a legacy CMakeLists that fails without this.
set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "Fix up libsamplerate CMake compatibility")

include(FetchContent)

if(SURGE_SRC_DIR)
    message(STATUS "kairos-surge: using local surge at ${SURGE_SRC_DIR}")
    set(_surge_root "${SURGE_SRC_DIR}")
else()
    message(STATUS "kairos-surge: fetching surge from GitHub — set SURGE_SRC_DIR to skip (this is slow)")
    FetchContent_Declare(surge_src
        GIT_REPOSITORY    https://github.com/surge-synthesizer/surge.git
        GIT_TAG           304f6dc27ad61686098e563611e1beb5163f5056
        GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_MakeAvailable(surge_src)
    set(_surge_root "${surge_src_SOURCE_DIR}")
endif()

# ---------------------------------------------------------------------------
# Surge build-time flags — mirror surge-rack's skip list.
# ---------------------------------------------------------------------------

set(SURGE_SKIP_JUCE_FOR_RACK    TRUE CACHE BOOL   "skip surge JUCE standalone build")
set(SURGE_SKIP_LUA              TRUE CACHE BOOL   "skip surge Lua build")
set(SURGE_SKIP_AIRWINDOWS       TRUE CACHE BOOL   "skip surge airwindows build")
set(SURGE_SKIP_ODDSOUND_MTS     TRUE CACHE BOOL   "skip surge Oddsound MTS build")
set(SURGE_SKIP_VST3             TRUE CACHE BOOL   "skip surge VST3 build")
set(SURGE_SKIP_ALSA             TRUE CACHE BOOL   "skip surge ALSA build")
set(SURGE_SKIP_STANDALONE       TRUE CACHE BOOL   "skip surge standalone build")
set(SURGE_COMPILE_BLOCK_SIZE    32   CACHE STRING "surge compile block size")

# Use surge's own bundled simde (avoids requiring the Rack SDK path).
set(SURGE_SIMDE_PATH "${_surge_root}/libs/simde" CACHE STRING "surge simde path")

add_subdirectory("${_surge_root}" "${CMAKE_CURRENT_BINARY_DIR}/surge-build" EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Warning suppression — surge targets trigger many diagnostics on modern
# compilers.  Targets are listed in the same order as surge-rack.
# ---------------------------------------------------------------------------

target_compile_options(juce_dsp_rack_sub PRIVATE
    -Wno-deprecated-copy
    -Wno-unused-function
    $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>
    $<$<CXX_COMPILER_ID:GNU>:-Wno-dangling-pointer>
    -DJUCE_AUDIOWORKGROUP_TYPES_AVAILABLE=0)

target_compile_options(fmt PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wno-dangling-reference>)

target_compile_options(eurorack PRIVATE
    -Wno-unused-local-typedefs
    $<$<PLATFORM_ID:Darwin>:-Wno-infinite-recursion>)

target_compile_options(sqlite PRIVATE -Wno-error)

target_compile_options(sst-plugininfra PRIVATE -Wno-unused-variable)

target_compile_options(sst-waveshapers INTERFACE -Wno-unused-variable)

target_compile_options(sst-filters INTERFACE
    -Wno-unused-variable
    $<$<CONFIG:Debug>:-Wno-sign-compare>)

target_compile_options(tinyxml PRIVATE
    -Wno-implicit-fallthrough
    -Wno-sign-compare)

target_compile_options(tuning-library INTERFACE
    $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>
    $<$<PLATFORM_ID:Darwin>:-Wno-unused-private-field>)

target_compile_options(surge-common PRIVATE
    -Wno-sign-compare
    -Wno-ignored-qualifiers
    -Wno-unused-value
    -Wno-uninitialized
    -Wno-implicit-fallthrough
    -Wno-char-subscripts
    -Wno-unused-local-typedefs
    -Wno-range-loop-construct
    -Wno-nonnull
    -Wno-reorder
    $<IF:$<CXX_COMPILER_ID:Clang>,-Wno-unused-const-variable,-Wno-unused-but-set-variable>
    $<IF:$<CXX_COMPILER_ID:GNU>,-Wno-stringop-truncation,-Wno-string-concatenation>
    $<IF:$<CXX_COMPILER_ID:GNU>,-Wno-class-memaccess,-Wno-class-varargs>
    $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>
    $<$<CXX_COMPILER_ID:GNU>:-Wno-dangling-reference>
    $<$<CXX_COMPILER_ID:GNU>:-Wno-restrict>
    $<$<PLATFORM_ID:Darwin>:-Wno-self-assign>
    $<$<PLATFORM_ID:Darwin>:-Wno-unused-lambda-capture>
    $<$<PLATFORM_ID:Darwin>:-Wno-mismatched-tags>
    $<$<PLATFORM_ID:Darwin>:-Wno-unused-but-set-variable>
    $<$<PLATFORM_ID:Darwin>:-Wno-infinite-recursion>)

# ---------------------------------------------------------------------------
# kairos_grid::surge — INTERFACE aggregate target.
# Links the same library set as surge-rack's SURGE_LIBS.
# ---------------------------------------------------------------------------

set(_surge_link_libs
    surge-common
    juce_dsp_rack_sub
    tinyxml
    strnatcmp
    samplerate
    sst-plugininfra
    fmt
    sqlite
    eurorack
    airwindows
    sst-basic-blocks
)

if(NOT WIN32)
    list(APPEND _surge_link_libs filesystem)
endif()

add_library(kairos-surge INTERFACE)
add_library(kairos_grid::surge ALIAS kairos-surge)
target_link_libraries(kairos-surge INTERFACE ${_surge_link_libs})

# Force-include our globals.h shadow before any translation unit so the
# include guard fires first; quoted #include "globals.h" in SurgeStorage.h
# would otherwise find the source-adjacent original before our compat copy.
# -fno-char8_t is required for C++20: SurgeStorage.h asserts u8string() ==
# std::string, which is only true when char8_t is suppressed.
# Both flags are safe and no-op in C++17.
if(MSVC)
    target_compile_options(kairos-surge INTERFACE
        /FI"${CMAKE_CURRENT_SOURCE_DIR}/compat_include/globals.h"
    )
else()
    target_compile_options(kairos-surge INTERFACE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/compat_include/globals.h"
        -fno-char8_t
    )
endif()

# juce_core.mm and juce_audio_formats.mm pull in macOS system frameworks.
if(APPLE)
    target_link_libraries(kairos-surge INTERFACE
        "-framework Foundation"
        "-framework AppKit"
        "-framework CoreServices"
        "-framework CoreAudio"
        "-framework CoreMIDI"
        "-framework Accelerate"
        "-framework IOKit"
        "-framework Security"
        "-framework AudioToolbox"
    )
endif()
