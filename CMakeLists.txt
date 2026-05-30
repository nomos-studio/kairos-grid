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
