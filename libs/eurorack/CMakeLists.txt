# SPDX-License-Identifier: MIT
#
# eurorack — vendored MI DSP library (Plaits + stmlib)
#
# Vendored from surge-synthesizer/eurorack @ f761446.
# Only the DSP code we use is included (plaits/dsp/, plaits/resources.*,
# stmlib/algorithms/, stmlib/dsp/, stmlib/fft/, stmlib/utils/).
# Firmware-only directories (bootloader/, drivers/, system/, midi/, ui/)
# are excluded.  All vendored code is MIT — Emilie Gillet / Mutable Instruments.
#
# Vendored to make the kairos-grid LGPL-2.1-or-later license pure:
# the upstream eurorack repo's LICENSE file references programming/serial
# (stm32loader, GPL-3) which is unrelated firmware tooling.  By vendoring
# only the MIT DSP subset we use, there is no GPL-3 code in the tree.

cmake_minimum_required(VERSION 3.20)
project(kairos-eurorack LANGUAGES CXX)

set(_er_src "${CMAKE_CURRENT_SOURCE_DIR}")

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

target_compile_options(kairos-eurorack PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wno-asm-operand-widths
        -Wno-unused-parameter
    >
)

if(MSVC)
    target_compile_options(kairos-eurorack PRIVATE
        /FImsvc_gcc_defines.h)
endif()

target_include_directories(kairos-eurorack
    PUBLIC
        ${_er_src}
        ${CMAKE_CURRENT_SOURCE_DIR}/special_include
)

set_target_properties(kairos-eurorack PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

add_library(kairos_grid::eurorack ALIAS kairos-eurorack)
