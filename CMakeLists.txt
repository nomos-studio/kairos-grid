# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)

project(kairos-grid
    VERSION 0.1.0
    DESCRIPTION "kairos-grid — sample-rate modular engine as a CLAP plugin"
    HOMEPAGE_URL "https://github.com/nomos-studio/kairos-grid"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Dependency policy
#
# - No Boost. C++20 STL covers all use cases.
# - FetchContent + pinned tag for any dependency with a build step.
# - FETCHCONTENT_UPDATES_DISCONNECTED: skip network on subsequent invocations.
# - Tests use Catch2 v3 (Boost Software License).
# - eurorack (MI DSP) is a git submodule in libs/eurorack/ — see that
#   directory's CMakeLists.txt and README for the platform shim details.
# - nomos-topology: header-only schema constants; pin to a commit SHA.
#   Set NOMOS_TOPOLOGY_SRC_DIR to a local checkout to skip the network fetch.
# ---------------------------------------------------------------------------

include(FetchContent)
include(GNUInstallDirs)
include(cmake/Sanitizers.cmake)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Skip network fetch if already populated")

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------

option(KAIROS_GRID_BUILD_TESTS  "Build kairos-grid unit tests"       OFF)
option(KAIROS_GRID_BUILD_ENGINE "Build the sample-rate engine library" ON)
option(KAIROS_GRID_BUILD_PLUGIN "Build the kairos-grid CLAP plugin"   OFF)
option(KAIROS_GRID_BUILD_MI     "Build MI DSP module wrappers (Plaits, SVF, OnePole)" OFF)
option(KAIROS_GRID_BUILD_SURGE  "Build Surge XT oscillator/filter/effect wrappers"    OFF)
option(KAIROS_GRID_BUILD_WASM   "Build Faust WASM module support (requires wasmtime)" OFF)

# ---------------------------------------------------------------------------
# nomos-topology — header-only EDN key constants (nomos::topology).
# Set NOMOS_TOPOLOGY_SRC_DIR to a local checkout to skip the network fetch.
# ---------------------------------------------------------------------------

if(DEFINED NOMOS_TOPOLOGY_SRC_DIR)
    FetchContent_Declare(nomos_topology SOURCE_DIR "${NOMOS_TOPOLOGY_SRC_DIR}")
else()
    FetchContent_Declare(nomos_topology
        GIT_REPOSITORY https://github.com/nomos-studio/nomos-topology.git
        GIT_TAG        01eef2f14e61e62a93ada455ca2a47263a90f3c5)
endif()
FetchContent_MakeAvailable(nomos_topology)

# ---------------------------------------------------------------------------
# kairos-grid-engine — pure C++20 engine; no CLAP SDK dependency.
# Suitable for unit testing and for linking into the CLAP plugin wrapper.
# ---------------------------------------------------------------------------

if(KAIROS_GRID_BUILD_ENGINE)

add_library(kairos-grid-engine STATIC
    src/grid_engine.cpp
    src/grid_graph.cpp
)

add_library(kairos_grid::engine ALIAS kairos-grid-engine)

target_include_directories(kairos-grid-engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        src
)

target_link_libraries(kairos-grid-engine
    PUBLIC
        nomos::topology
)

nomos_sanitize_target(kairos-grid-engine)

set_target_properties(kairos-grid-engine PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    target_compile_options(kairos-grid-engine PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    )
endif()

endif() # KAIROS_GRID_BUILD_ENGINE

# ---------------------------------------------------------------------------
# CLAP SDK — declared here, before MI and Surge, so that Surge's
# clap-juce-extensions dependency sees an already-populated FetchContent
# entry and skips its own fetch.  The `clap` INTERFACE target is created
# exactly once; the PLUGIN block below just uses it.
# ---------------------------------------------------------------------------

if(KAIROS_GRID_BUILD_PLUGIN OR KAIROS_GRID_BUILD_SURGE)
    if(DEFINED CLAP_SRC_DIR)
        FetchContent_Declare(clap SOURCE_DIR "${CLAP_SRC_DIR}")
    else()
        FetchContent_Declare(clap
            GIT_REPOSITORY https://github.com/free-audio/clap.git
            GIT_TAG        1.2.6)
    endif()
    FetchContent_MakeAvailable(clap)
endif()

# ---------------------------------------------------------------------------
# wasmtime C API — pre-built binaries, downloaded only when KAIROS_GRID_BUILD_WASM=ON.
#
# Platform detection and versioning mirrors kairos/CMakeLists.txt; keep them
# in sync when updating the wasmtime version.
# ---------------------------------------------------------------------------

if(KAIROS_GRID_BUILD_WASM AND KAIROS_GRID_BUILD_ENGINE)

    set(_wasmtime_ver "v44.0.1")

    if(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(_wasmtime_platform "aarch64-macos")
            set(_wasmtime_hash "SHA256=5da6d09fa6340db6d7afc8b73d189eff4973825c9a6fa5e814d2f9b1ddfd8998")
        else()
            set(_wasmtime_platform "x86_64-macos")
            set(_wasmtime_hash "SHA256=8a3877178c50a2a2a20fcb94ca86efd14ba4376698e48a509f72bc52e961ce49")
        endif()
    else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(_wasmtime_platform "aarch64-linux")
            set(_wasmtime_hash "SHA256=5b4614a311c7c391fb1fc5e9c7ab3782f5a441f9e1fa0cc60fe364ff5371c010")
        else()
            set(_wasmtime_platform "x86_64-linux")
            set(_wasmtime_hash "SHA256=fe9b4bfa87724054cebcc09b83a17d5ded66a465054b77c8de0ebc166ad78d7a")
        endif()
    endif()

    set(_wasmtime_name "wasmtime-${_wasmtime_ver}-${_wasmtime_platform}-c-api")
    FetchContent_Declare(wasmtime_grid
        URL "https://github.com/bytecodealliance/wasmtime/releases/download/${_wasmtime_ver}/${_wasmtime_name}.tar.xz"
        URL_HASH "${_wasmtime_hash}")
    FetchContent_MakeAvailable(wasmtime_grid)

    add_library(kairos-grid-wasmtime INTERFACE)
    target_include_directories(kairos-grid-wasmtime INTERFACE
        "${wasmtime_grid_SOURCE_DIR}/include")
    target_link_libraries(kairos-grid-wasmtime INTERFACE
        "${wasmtime_grid_SOURCE_DIR}/lib/libwasmtime.a")
    if(APPLE)
        target_link_libraries(kairos-grid-wasmtime INTERFACE
            "-framework Security" "-framework CoreFoundation" "-framework SystemConfiguration")
    else()
        target_link_libraries(kairos-grid-wasmtime INTERFACE pthread dl)
    endif()

    # Static library carrying WasmGridModule; links against the engine + wasmtime.
    add_library(kairos-grid-wasm STATIC
        src/wasm_grid_module.cpp
    )
    add_library(kairos_grid::wasm ALIAS kairos-grid-wasm)

    # wasmtime is PUBLIC so that consumers (plugin, tests) get libwasmtime.a
    # transitively without having to repeat the link dependency.
    target_link_libraries(kairos-grid-wasm
        PUBLIC
            kairos_grid::engine
            kairos-grid-wasmtime
    )

    set_target_properties(kairos-grid-wasm PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
        target_compile_options(kairos-grid-wasm PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
        )
    endif()

endif() # KAIROS_GRID_BUILD_WASM

# ---------------------------------------------------------------------------
# MI DSP wrappers — Plaits macro-oscillator, stmlib SVF, OnePole.
# Requires the eurorack static library (GPL-3.0, MIT code within).
# ---------------------------------------------------------------------------

if(KAIROS_GRID_BUILD_MI AND KAIROS_GRID_BUILD_ENGINE)

    # Point EURORACK_SRC_DIR at a local checkout to skip the network fetch:
    #   cmake --preset dev -DEURORACK_SRC_DIR=/path/to/surge/libs/eurorack
    add_subdirectory(libs/eurorack)

    # INTERFACE convenience target — links engine + eurorack so consumers of
    # the MI headers get all required include paths and link deps in one line.
    add_library(kairos-grid-mi INTERFACE)
    add_library(kairos_grid::mi ALIAS kairos-grid-mi)

    target_link_libraries(kairos-grid-mi INTERFACE
        kairos_grid::engine
        kairos_grid::eurorack
    )

endif() # KAIROS_GRID_BUILD_MI

# ---------------------------------------------------------------------------
# Surge XT DSP wrappers — oscillators, filters, effects.
# Brings in surge-common and all required support libs via libs/surge.
# ---------------------------------------------------------------------------

if(KAIROS_GRID_BUILD_SURGE AND KAIROS_GRID_BUILD_ENGINE)

    # Point SURGE_SRC_DIR at a local surge checkout to skip the network fetch:
    #   cmake --preset surge -DSURGE_SRC_DIR=/path/to/surge
    add_subdirectory(libs/surge)

    # INTERFACE convenience target — exposes engine headers + all surge link deps.
    add_library(kairos-grid-surge INTERFACE)
    add_library(kairos_grid::surge-modules ALIAS kairos-grid-surge)

    target_link_libraries(kairos-grid-surge INTERFACE
        kairos_grid::engine
        kairos_grid::surge
    )

endif() # KAIROS_GRID_BUILD_SURGE

# ---------------------------------------------------------------------------
# CLAP plugin wrapper — adds CLAP SDK dependency.
# Brings in the engine library transitively.
# ---------------------------------------------------------------------------

if(KAIROS_GRID_BUILD_PLUGIN AND KAIROS_GRID_BUILD_ENGINE)

    # CLAP is already declared and made available at the top of this file.
    # No FetchContent call needed here.

    # CLAP plugin shared library.
    add_library(kairos-grid MODULE
        src/clap_plugin.cpp
    )

    set_target_properties(kairos-grid PROPERTIES
        PREFIX          ""
        SUFFIX          ".clap"
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    target_link_libraries(kairos-grid PRIVATE
        kairos_grid::engine
        clap
        sanitizers::sanitizers
    )

    # Link MI DSP wrappers into the plugin when both are enabled.
    # clap_plugin.cpp detects availability via KAIROS_GRID_PLUGIN_HAS_MI.
    if(TARGET kairos_grid::mi)
        target_link_libraries(kairos-grid PRIVATE kairos_grid::mi)
        target_compile_definitions(kairos-grid PRIVATE KAIROS_GRID_PLUGIN_HAS_MI=1)
    endif()

    # Link Surge XT wrappers into the plugin when both are enabled.
    # clap_plugin.cpp detects availability via KAIROS_GRID_PLUGIN_HAS_SURGE.
    if(TARGET kairos_grid::surge-modules)
        target_link_libraries(kairos-grid PRIVATE kairos_grid::surge-modules)
        target_compile_definitions(kairos-grid PRIVATE KAIROS_GRID_PLUGIN_HAS_SURGE=1)
    endif()

    # Link WASM module support into the plugin when enabled.
    # clap_plugin.cpp detects availability via KAIROS_GRID_PLUGIN_HAS_WASM.
    if(TARGET kairos_grid::wasm)
        target_link_libraries(kairos-grid PRIVATE kairos_grid::wasm)
        target_compile_definitions(kairos-grid PRIVATE KAIROS_GRID_PLUGIN_HAS_WASM=1)
    endif()

    if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
        target_compile_options(kairos-grid PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
        )
    endif()

endif() # KAIROS_GRID_BUILD_PLUGIN

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(KAIROS_GRID_BUILD_TESTS AND KAIROS_GRID_BUILD_ENGINE)
    enable_testing()

    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)

    add_subdirectory(tests)
endif()

# REUSE licence compliance target
find_program(REUSE_EXECUTABLE reuse)
if(REUSE_EXECUTABLE AND NOT TARGET lint-reuse)
    add_custom_target(lint-reuse
        COMMAND ${REUSE_EXECUTABLE} lint
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking REUSE compliance"
        VERBATIM
    )
    add_test(NAME reuse-lint
        COMMAND ${REUSE_EXECUTABLE} lint
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endif()
