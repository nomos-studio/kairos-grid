# SPDX-License-Identifier: GPL-3.0-or-later
#
# eurorack — MI DSP library (Plaits + stmlib)
#
# This wrapper mirrors the approach in surge/libs/eurorack/CMakeLists.txt
# from github.com/surge-synthesizer/surge-rack, adapted for FetchContent use.
#
# Set EURORACK_SRC_DIR to the root of a pre-cloned surge-synthesizer/eurorack
# checkout (the directory that contains the nested eurorack/ source tree) to
# avoid a network fetch.  If unset, FetchContent downloads commit f761446.
#
# The resulting target is kairos_grid::eurorack.

cmake_minimum_required(VERSION 3.20)
project(kairos-eurorack LANGUAGES CXX)

include(FetchContent)

if(EURORACK_SRC_DIR)
    message(STATUS "kairos-eurorack: using local eurorack at ${EURORACK_SRC_DIR}")
    set(_er_root "${EURORACK_SRC_DIR}")
else()
    FetchContent_Declare(eurorack_src
        GIT_REPOSITORY https://github.com/surge-synthesizer/eurorack.git
        GIT_TAG        f761446)
    FetchContent_MakeAvailable(eurorack_src)
    set(_er_root "${eurorack_src_SOURCE_DIR}")
endif()

# Source paths relative to _er_root/eurorack/
set(_er_src "${_er_root}/eurorack")

add_library(kairos-eurorack STATIC
    # Plaits voice + all synthesis engines
    ${_er_src}/plaits/dsp/voice.cc
    ${_er_src}/plaits/dsp/speech/lpc_speech_synth_phonemes.cc
    ${_er_src}/plaits/dsp/speech/lpc_speech_synth_controller.cc
    ${_er_src}/plaits/dsp/speech/lpc_speech_synth.cc
    ${_er_src}/plaits/dsp/speech/naive_speech_synth.cc
    ${_er_src}/plaits/dsp/speech/lpc_speech_synth_words.cc
    ${_er_src}/plaits/dsp/speech/sam_speech_synth.cc
    ${_er_src}/plaits/dsp/physical_modelling/string.cc
    ${_er_src}/plaits/dsp/physical_modelling/resonator.cc
    ${_er_src}/plaits/dsp/physical_modelling/string_voice.cc
    ${_er_src}/plaits/dsp/physical_modelling/modal_voice.cc
    ${_er_src}/plaits/dsp/engine/fm_engine.cc
    ${_er_src}/plaits/dsp/engine/swarm_engine.cc
    ${_er_src}/plaits/dsp/engine/wavetable_engine.cc
    ${_er_src}/plaits/dsp/engine/particle_engine.cc
    ${_er_src}/plaits/dsp/engine/noise_engine.cc
    ${_er_src}/plaits/dsp/engine/hi_hat_engine.cc
    ${_er_src}/plaits/dsp/engine/chord_engine.cc
    ${_er_src}/plaits/dsp/engine/waveshaping_engine.cc
    ${_er_src}/plaits/dsp/engine/string_engine.cc
    ${_er_src}/plaits/dsp/engine/modal_engine.cc
    ${_er_src}/plaits/dsp/engine/grain_engine.cc
    ${_er_src}/plaits/dsp/engine/bass_drum_engine.cc
    ${_er_src}/plaits/dsp/engine/speech_engine.cc
    ${_er_src}/plaits/dsp/engine/additive_engine.cc
    ${_er_src}/plaits/dsp/engine/virtual_analog_engine.cc
    ${_er_src}/plaits/dsp/engine/snare_drum_engine.cc
    ${_er_src}/plaits/resources.cc
    # stmlib runtime
    ${_er_src}/stmlib/dsp/units.cc
    ${_er_src}/stmlib/dsp/atan.cc
    ${_er_src}/stmlib/utils/random.cc
)

target_compile_definitions(kairos-eurorack
    PRIVATE
        TEST=1
        $<$<BOOL:${WIN32}>:_USE_MATH_DEFINES>
)

# Force-include the MSVC shim only on MSVC; GCC/Clang understand __attribute__.
if(MSVC)
    target_compile_options(kairos-eurorack PRIVATE
        /FImsvc_gcc_defines.h)
endif()

target_include_directories(kairos-eurorack
    PUBLIC
        # Headers are at <_er_root>/eurorack/stmlib/... and .../plaits/...
        # Include root is _er_src (the directory containing stmlib/ and plaits/).
        ${_er_src}
        # Platform shim (force-included on MSVC; harmless to have on path).
        ${CMAKE_CURRENT_SOURCE_DIR}/special_include
)

set_target_properties(kairos-eurorack PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

add_library(kairos_grid::eurorack ALIAS kairos-eurorack)
