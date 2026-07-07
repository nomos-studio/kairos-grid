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
        sanitizers::sanitizers
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
            sanitizers::sanitizers
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
            sanitizers::sanitizers
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
            sanitizers::sanitizers
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
            sanitizers::sanitizers
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
    set(_plugin_test_sources
        test_clap_plugin.cpp
        test_tap_bus_ext.cpp
        test_param_bus_ext.cpp
        test_patch_bus_ext.cpp
        test_concurrent.cpp
        ../src/clap_plugin.cpp
    )
    set(_plugin_test_libs
        kairos_grid::engine
        clap
        Catch2::Catch2WithMain
        sanitizers::sanitizers
    )

    # When WASM is also enabled, include the hot-swap extension tests and link
    # kairos_grid::wasm so that clap_plugin.cpp compiles with KAIROS_GRID_PLUGIN_HAS_WASM.
    if(TARGET kairos_grid::wasm)
        list(APPEND _plugin_test_sources test_hot_swap_ext.cpp)
        list(APPEND _plugin_test_libs    kairos_grid::wasm)
    endif()

    add_executable(kairos-grid-plugin-tests ${_plugin_test_sources})

    target_link_libraries(kairos-grid-plugin-tests PRIVATE ${_plugin_test_libs})

    if(TARGET kairos_grid::wasm)
        target_compile_definitions(kairos-grid-plugin-tests PRIVATE
            KAIROS_GRID_PLUGIN_HAS_WASM=1)
    endif()

    set_target_properties(kairos-grid-plugin-tests PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    catch_discover_tests(kairos-grid-plugin-tests)
endif()

# WASM module tests — only built when kairos_grid::wasm is available.
if(TARGET kairos_grid::wasm)
    add_executable(kairos-grid-wasm-tests
        test_wasm_module.cpp
    )

    target_link_libraries(kairos-grid-wasm-tests
        PRIVATE
            kairos_grid::wasm
            Catch2::Catch2WithMain
            sanitizers::sanitizers
    )

    # Provide the absolute path to the vendored test fixtures so the integration
    # tests can locate sine_stereo.wasm regardless of where ctest is invoked from.
    target_compile_definitions(kairos-grid-wasm-tests PRIVATE
        KAIROS_GRID_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

    set_target_properties(kairos-grid-wasm-tests PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    catch_discover_tests(kairos-grid-wasm-tests)
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
            sanitizers::sanitizers
    )

    set_target_properties(kairos-grid-mi-tests PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    catch_discover_tests(kairos-grid-mi-tests)
endif()
