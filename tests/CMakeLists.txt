# SPDX-License-Identifier: GPL-3.0-or-later
include(Catch)

add_executable(kairos-grid-tests
    test_grid_engine.cpp
)

target_link_libraries(kairos-grid-tests
    PRIVATE
        kairos_grid::engine
        Catch2::Catch2WithMain
)

set_target_properties(kairos-grid-tests PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

catch_discover_tests(kairos-grid-tests)
