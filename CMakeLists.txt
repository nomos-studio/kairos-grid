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
# CLAP plugin wrapper — adds CLAP SDK dependency.
# Brings in the engine library transitively.
# ---------------------------------------------------------------------------

if(KAIROS_GRID_BUILD_PLUGIN AND KAIROS_GRID_BUILD_ENGINE)

    FetchContent_Declare(clap
        GIT_REPOSITORY https://github.com/free-audio/clap.git
        GIT_TAG        1.2.6
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(clap)

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
