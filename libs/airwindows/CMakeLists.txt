# SPDX-License-Identifier: GPL-3.0-or-later
#
# airwindows — header-only Airwindows DSP algorithms.
#
# Extracts per-channel DSP state structs from Chris Johnson's public domain /
# MIT-licensed Airwindows plugins.  No sources; INTERFACE library only.
#
# The resulting target is kairos_grid::airwindows.

cmake_minimum_required(VERSION 3.20)
project(kairos-airwindows LANGUAGES CXX)

add_library(kairos-airwindows INTERFACE)
add_library(kairos_grid::airwindows ALIAS kairos-airwindows)

target_include_directories(kairos-airwindows INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
