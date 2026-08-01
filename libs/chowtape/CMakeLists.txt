# SPDX-License-Identifier: GPL-3.0-or-later
# ChowTape DSP — JUCE-free vendored static library.
# Source: jatinchowdhury18/AnalogTapeModel (GPL-3.0)
# Compat shim: compat/ (LGPL-2.1-or-later)

cmake_minimum_required(VERSION 3.21)

add_library(kairos-grid-chowtape STATIC
    Processors/Hysteresis/HysteresisProcessing.cpp
    Processors/Timing_Effects/WowProcess.cpp
    Processors/Timing_Effects/FlutterProcess.cpp
    Processors/Timing_Effects/WowFlutterProcessor.cpp
    Processors/Loss_Effects/LossFilter.cpp
    Processors/Loss_Effects/AzimuthProc.cpp
    Processors/Compression/CompressionProcessor.cpp
    Processors/Input_Filters/InputFilters.cpp
)

add_library(kairos_grid::chowtape ALIAS kairos-grid-chowtape)

# PUBLIC: extension.cpp (in src/kairos-grid-tape) uses #include "Processors/..."
target_include_directories(kairos-grid-chowtape
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(kairos-grid-chowtape PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
    POSITION_INDEPENDENT_CODE ON  # required for linking into a shared .kgext
)

# Silence clang-tidy noise on vendored code
target_compile_options(kairos-grid-chowtape PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>
)
