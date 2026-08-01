# SPDX-License-Identifier: GPL-3.0-or-later
# kairos-grid-tape.kgext — ChowTape hysteresis/loss/flutter extension.
# GPL-3.0 because AnalogTapeModel (jatinchowdhury18) is GPL-3.0.
# Loaded at runtime by the kairos-grid CLAP plugin via the .kgext mechanism.

add_library(kairos-grid-tape-ext SHARED src/extension.cpp)

add_library(kairos_grid::tape-ext ALIAS kairos-grid-tape-ext)

set_target_properties(kairos-grid-tape-ext PROPERTIES
    OUTPUT_NAME           kairos-grid-tape
    PREFIX                ""
    SUFFIX                ".kgext"
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_link_libraries(kairos-grid-tape-ext PRIVATE
    kairos_grid::engine
    kairos_grid::chowtape
)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    target_compile_options(kairos-grid-tape-ext PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    )
endif()
