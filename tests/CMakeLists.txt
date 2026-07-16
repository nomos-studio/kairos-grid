# SPDX-License-Identifier: GPL-3.0-or-later
include(Catch)

add_executable(kairos-grid-tests
    test_grid_engine.cpp
    test_environment_module.cpp
    test_audio_modules.cpp
    test_shaper_module.cpp
    test_peek_module.cpp
    test_poke_module.cpp
    test_sah_module.cpp
    test_buffer_module.cpp
    test_clock_division_module.cpp
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

    add_executable(kairos-grid-surge-waveshaper-tests
        test_surge_waveshaper_module.cpp
    )

    target_link_libraries(kairos-grid-surge-waveshaper-tests
        PRIVATE
            kairos_grid::surge-modules
            Catch2::Catch2WithMain
            sanitizers::sanitizers
    )

    set_target_properties(kairos-grid-surge-waveshaper-tests PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    catch_discover_tests(kairos-grid-surge-waveshaper-tests)
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

# M7g registry integration tests — exercises newly registered module types through
# the full EDN patch-bus → param-bus roundtrip.  Each test section is individually
# guarded by KAIROS_GRID_PLUGIN_HAS_MI / KAIROS_GRID_PLUGIN_HAS_SURGE so only
# what is actually linked in gets compiled.
#
# Requires at minimum: clap + kairos_grid::mi (plugin-mi preset).
# Surge registry tests activate when kairos_grid::surge-modules is also present
# (future plugin-mi-surge preset — currently blocked by CLAP target collision
# between clap-juce-extensions and the plugin preset's FetchContent clap target).
if(TARGET clap AND TARGET kairos_grid::mi)
    set(_m7g_test_sources
        test_m7g_registry.cpp
        ../src/clap_plugin.cpp
    )
    set(_m7g_test_libs
        kairos_grid::engine
        kairos_grid::mi
        clap
        Catch2::Catch2WithMain
        sanitizers::sanitizers
    )
    set(_m7g_test_defs
        KAIROS_GRID_PLUGIN_HAS_MI=1
    )

    if(TARGET kairos_grid::surge-modules)
        list(APPEND _m7g_test_libs kairos_grid::surge-modules)
        list(APPEND _m7g_test_defs KAIROS_GRID_PLUGIN_HAS_SURGE=1)
    endif()

    if(TARGET kairos_grid::wasm)
        list(APPEND _m7g_test_libs kairos_grid::wasm)
        list(APPEND _m7g_test_defs KAIROS_GRID_PLUGIN_HAS_WASM=1)
    endif()

    add_executable(kairos-grid-m7g-registry-tests ${_m7g_test_sources})
    target_link_libraries(kairos-grid-m7g-registry-tests PRIVATE ${_m7g_test_libs})
    target_compile_definitions(kairos-grid-m7g-registry-tests PRIVATE ${_m7g_test_defs})

    set_target_properties(kairos-grid-m7g-registry-tests PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    catch_discover_tests(kairos-grid-m7g-registry-tests)
endif()

# Airwindows saturation tests — only built when kairos_grid::airwindows-modules is available.
if(TARGET kairos_grid::airwindows-modules)
    add_executable(kairos-grid-airwindows-tests
        test_airwindows_module.cpp
    )

    target_link_libraries(kairos-grid-airwindows-tests
        PRIVATE
            kairos_grid::airwindows-modules
            Catch2::Catch2WithMain
            sanitizers::sanitizers
    )

    set_target_properties(kairos-grid-airwindows-tests PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    catch_discover_tests(kairos-grid-airwindows-tests)
endif()

# FFT spectral analysis tests — only built when kairos_grid::fft-modules is available.
if(TARGET kairos_grid::fft-modules)
    add_executable(kairos-grid-fft-tests
        test_fft_module.cpp
    )

    target_link_libraries(kairos-grid-fft-tests
        PRIVATE
            kairos_grid::fft-modules
            Catch2::Catch2WithMain
            sanitizers::sanitizers
    )

    set_target_properties(kairos-grid-fft-tests PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    catch_discover_tests(kairos-grid-fft-tests)
endif()

# WDF circuit model tests — only built when kairos_grid::wdf-modules is available.
if(TARGET kairos_grid::wdf-modules)
    add_executable(kairos-grid-wdf-tests
        test_wdf_modules.cpp
    )

    target_link_libraries(kairos-grid-wdf-tests
        PRIVATE
            kairos_grid::wdf-modules
            Catch2::Catch2WithMain
            sanitizers::sanitizers
    )

    set_target_properties(kairos-grid-wdf-tests PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    catch_discover_tests(kairos-grid-wdf-tests)
endif()

# MI DSP tests — only built when kairos_grid::mi is available.
if(TARGET kairos_grid::mi)
    add_executable(kairos-grid-mi-tests
        test_mi_modules.cpp
        test_waveshaper_module.cpp
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
