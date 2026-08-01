# SPDX-License-Identifier: LGPL-2.1-or-later
# kairos-grid-mi.kgext — MI DSP module extension for kairos-grid.
# MIT-licensed DSP code (Emilie Gillet / Mutable Instruments); LGPL-2.1-or-later wrappers.
# Loaded at runtime by the kairos-grid CLAP plugin via the .kgext extension mechanism.

add_library(kairos-grid-mi-ext SHARED src/extension.cpp)

set_target_properties(kairos-grid-mi-ext PROPERTIES
    OUTPUT_NAME           kairos-grid-mi
    PREFIX                ""
    SUFFIX                ".kgext"
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_link_libraries(kairos-grid-mi-ext PRIVATE
    kairos_grid::engine
    kairos_grid::eurorack
)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    target_compile_options(kairos-grid-mi-ext PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    )
endif()
