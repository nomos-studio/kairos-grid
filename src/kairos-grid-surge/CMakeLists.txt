# SPDX-License-Identifier: GPL-3.0-or-later
# kairos-grid-surge.kgext — Surge XT module extension for kairos-grid.
# GPL-3.0 because SurgeXT and its dependencies are GPL-3.0.
# Loaded at runtime by the kairos-grid CLAP plugin via the .kgext extension mechanism.

add_library(kairos-grid-surge-ext SHARED src/extension.cpp)

set_target_properties(kairos-grid-surge-ext PROPERTIES
    OUTPUT_NAME           kairos-grid-surge
    PREFIX                ""
    SUFFIX                ".kgext"
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_link_libraries(kairos-grid-surge-ext PRIVATE
    kairos_grid::engine
    kairos_grid::surge
)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    target_compile_options(kairos-grid-surge-ext PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    )
endif()
