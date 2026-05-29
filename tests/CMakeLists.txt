# SPDX-License-Identifier: GPL-3.0-or-later
include(Catch)

add_executable(kairos-grid-tests
    test_grid_engine.cpp
    test_environment_module.cpp
    test_audio_modules.cpp
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

# Surge XT oscillator tests — only built when kairos_grid::surge-modules is available.
if(TARGET kairos_grid::surge-modules)
    add_executable(kairos-grid-surge-tests
        test_surge_modules.cpp
    )

    target_link_libraries(kairos-grid-surge-tests
        PRIVATE
            kairos_grid::surge-modules
            Catch2::Catch2WithMain
    )

    set_target_properties(kairos-grid-surge-tests PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    catch_discover_tests(kairos-grid-surge-tests)

    add_executable(kairos-grid-surge-filter-tests
        test_surge_filter_modules.cpp
    )

    target_link_libraries(kairos-grid-surge-filter-tests
        PRIVATE
            kairos_grid::surge-modules
            Catch2::Catch2WithMain
    )

    set_target_properties(kairos-grid-surge-filter-tests PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    catch_discover_tests(kairos-grid-surge-filter-tests)

    add_executable(kairos-grid-surge-effect-tests
        test_surge_effect_modules.cpp
    )

    target_link_libraries(kairos-grid-surge-effect-tests
        PRIVATE
            kairos_grid::surge-modules
            Catch2::Catch2WithMain
    )

    set_target_properties(kairos-grid-surge-effect-tests PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    catch_discover_tests(kairos-grid-surge-effect-tests)

    add_executable(kairos-grid-surge-modulator-tests
        test_surge_modulator_modules.cpp
    )

    target_link_libraries(kairos-grid-surge-modulator-tests
        PRIVATE
            kairos_grid::surge-modules
            Catch2::Catch2WithMain
    )

    set_target_properties(kairos-grid-surge-modulator-tests PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    catch_discover_tests(kairos-grid-surge-modulator-tests)
endif()

# CLAP plugin tests — only built when the clap target is available (plugin preset).
if(TARGET clap)
    add_executable(kairos-grid-plugin-tests
        test_clap_plugin.cpp
        test_tap_bus_ext.cpp
        ../src/clap_plugin.cpp
    )

    target_link_libraries(kairos-grid-plugin-tests
        PRIVATE
            kairos_grid::engine
            clap
            Catch2::Catch2WithMain
    )

    set_target_properties(kairos-grid-plugin-tests PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    catch_discover_tests(kairos-grid-plugin-tests)
endif()

# MI DSP tests — only built when kairos_grid::mi is available.
if(TARGET kairos_grid::mi)
    add_executable(kairos-grid-mi-tests
        test_mi_modules.cpp
    )

    target_link_libraries(kairos-grid-mi-tests
        PRIVATE
            kairos_grid::mi
            Catch2::Catch2WithMain
    )

    set_target_properties(kairos-grid-mi-tests PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    catch_discover_tests(kairos-grid-mi-tests)
endif()
