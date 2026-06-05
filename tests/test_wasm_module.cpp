// SPDX-License-Identifier: GPL-3.0-or-later
//
// WasmGridModule tests — error-path smoke tests and fixture integration tests.
//
// The integration tests require the vendored sine_stereo.wasm fixture
// (tests/fixtures/sine_stereo.wasm + sine_stereo.json).
// KAIROS_GRID_TEST_FIXTURE_DIR is set by CMake to the absolute path of the
// tests/fixtures/ directory.

#include <kairos_grid/wasm_grid_module.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdio>
#include <string>

using namespace kairos_grid;

// ---------------------------------------------------------------------------
// Error-path smoke tests — require no real .wasm file
// ---------------------------------------------------------------------------

TEST_CASE("WasmGridModule::create returns nullptr for empty path", "[wasm]") {
    auto m = WasmGridModule::create("");
    REQUIRE(m == nullptr);
}

TEST_CASE("WasmGridModule::create returns nullptr for nonexistent file", "[wasm]") {
    auto m = WasmGridModule::create("/nonexistent/path/that/does/not/exist.wasm");
    REQUIRE(m == nullptr);
}

TEST_CASE("WasmGridModule::create returns nullptr for invalid wasm bytes", "[wasm]") {
    const char* path = "/tmp/kairos_grid_test_invalid.wasm";
    if (FILE* f = std::fopen(path, "wb")) {
        const char garbage[] = "this is not a wasm file\n";
        std::fwrite(garbage, 1, sizeof(garbage) - 1, f);
        std::fclose(f);
    }
    auto m = WasmGridModule::create(path);
    REQUIRE(m == nullptr);
    std::remove(path);
}

// ---------------------------------------------------------------------------
// Integration tests — sine_stereo.wasm fixture
//
// Fixture: tests/fixtures/sine_stereo.dsp compiled with faust -lang wasm.
// DSP: stereo sine oscillator; 0 audio inputs, 2 audio outputs, 3 params.
// Params (Faust JSON order): amp, detune, freq.
// ---------------------------------------------------------------------------

#ifndef KAIROS_GRID_TEST_FIXTURE_DIR
#define KAIROS_GRID_TEST_FIXTURE_DIR "."
#endif

static const std::string k_fixture_wasm =
    std::string(KAIROS_GRID_TEST_FIXTURE_DIR) + "/sine_stereo.wasm";

TEST_CASE("WasmGridModule::create succeeds for sine_stereo fixture", "[wasm][integration]") {
    auto m = WasmGridModule::create(k_fixture_wasm);
    REQUIRE(m != nullptr);
}

TEST_CASE("WasmGridModule: sine_stereo port layout", "[wasm][integration]") {
    auto m = WasmGridModule::create(k_fixture_wasm);
    REQUIRE(m != nullptr);

    // Fixture: 0 audio in, 2 audio out, 3 params.
    REQUIRE(m->n_audio_in()  == 0);
    REQUIRE(m->n_audio_out() == 2);
    REQUIRE(m->n_params()    == 3);

    // Total GridModule ports.
    REQUIRE(m->inputs.size()  == 3u); // 0 audio + 3 param
    REQUIRE(m->outputs.size() == 2u);

    // param_ports: raw labels before stem prefix is applied.
    // Faust emits params in JSON order (alphabetical for hslider): amp, detune, freq.
    REQUIRE(m->param_ports.size() == 3u);
    REQUIRE(m->param_ports[0].name == "amp");
    REQUIRE(m->param_ports[1].name == "detune");
    REQUIRE(m->param_ports[2].name == "freq");

    // Each param port index points into inputs[].
    REQUIRE(m->param_ports[0].port_idx == 0);
    REQUIRE(m->param_ports[1].port_idx == 1);
    REQUIRE(m->param_ports[2].port_idx == 2);
}

TEST_CASE("WasmGridModule: prepare does not throw or crash", "[wasm][integration]") {
    auto m = WasmGridModule::create(k_fixture_wasm);
    REQUIRE(m != nullptr);

    const GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    REQUIRE_NOTHROW(m->prepare(args));

    // Second prepare() (sample-rate change) must also be safe.
    const GridProcessArgs args2{44100.f, 1.f / 44100.f, 0};
    REQUIRE_NOTHROW(m->prepare(args2));
}

TEST_CASE("WasmGridModule: process produces non-zero output", "[wasm][integration]") {
    auto m = WasmGridModule::create(k_fixture_wasm);
    REQUIRE(m != nullptr);

    const GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    m->prepare(args);

    // Param port indices: 0=amp, 1=detune, 2=freq (alphabetical Faust JSON order).
    m->inputs[0].voltage = 1.0f;   // amp   = 1.0
    m->inputs[1].voltage = 0.0f;   // detune = 0.0
    m->inputs[2].voltage = 440.0f; // freq  = 440 Hz

    // Run 200 samples (~4 ms at 48 kHz). The first sample from os.osc starts
    // at phase 0, so the very first output is 0; accumulate to catch non-zero.
    float sum_abs = 0.f;
    for (int i = 0; i < 200; ++i) {
        m->process(args);
        sum_abs += std::abs(m->outputs[0].voltage);
        sum_abs += std::abs(m->outputs[1].voltage);
    }

    REQUIRE(sum_abs > 0.f);
}

TEST_CASE("WasmGridModule: left and right outputs diverge with detune", "[wasm][integration]") {
    auto m = WasmGridModule::create(k_fixture_wasm);
    REQUIRE(m != nullptr);

    const GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    m->prepare(args);

    m->inputs[0].voltage = 1.0f;    // amp
    m->inputs[1].voltage = 0.01f;   // detune = 1%
    m->inputs[2].voltage = 440.0f;  // freq

    // Run long enough for the phase difference to accumulate visibly.
    float diff_sum = 0.f;
    for (int i = 0; i < 4800; ++i) {
        m->process(args);
        diff_sum += std::abs(m->outputs[0].voltage - m->outputs[1].voltage);
    }

    // With 1% detuning the two oscillators diverge — their sum of differences
    // must be strictly positive over 4800 samples.
    REQUIRE(diff_sum > 0.f);
}
