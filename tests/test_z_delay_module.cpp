// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for ZDelayModule (z-1 unit delay).

#include <kairos_grid/grid_graph.hpp>
#include <kairos_grid/z_delay_module.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace kairos_grid;

static GridProcessArgs make_args() {
    return {48000.f, 1.f / 48000.f, 0};
}

// ---------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------

TEST_CASE("ZDelayModule: port counts") {
    ZDelayModule m;
    REQUIRE(m.inputs.size() == 1);
    REQUIRE(m.outputs.size() == 1);
    REQUIRE(m.param_ports.empty());
    REQUIRE(m.taps.empty());
}

// ---------------------------------------------------------------------------
// process() — direct, no engine
// ---------------------------------------------------------------------------

TEST_CASE("ZDelayModule: output is zero before any input is presented") {
    ZDelayModule    m;
    GridProcessArgs args = make_args();
    m.prepare(args);

    m.inputs[0].voltage = 1.0f;
    m.process(args);

    // First sample: output is the initial register value (0.0), not the input.
    REQUIRE_THAT(m.outputs[0].voltage, Catch::Matchers::WithinAbs(0.f, 1e-9f));
}

TEST_CASE("ZDelayModule: output lags input by exactly one sample") {
    ZDelayModule    m;
    GridProcessArgs args = make_args();
    m.prepare(args);

    // Sample 0: write 0.5, read 0.0 (initial register)
    m.inputs[0].voltage = 0.5f;
    m.process(args);
    REQUIRE_THAT(m.outputs[0].voltage, Catch::Matchers::WithinAbs(0.0f, 1e-9f));

    // Sample 1: write 0.75, read 0.5 (previous input)
    m.inputs[0].voltage = 0.75f;
    m.process(args);
    REQUIRE_THAT(m.outputs[0].voltage, Catch::Matchers::WithinAbs(0.5f, 1e-6f));

    // Sample 2: write -1.0, read 0.75
    m.inputs[0].voltage = -1.0f;
    m.process(args);
    REQUIRE_THAT(m.outputs[0].voltage, Catch::Matchers::WithinAbs(0.75f, 1e-6f));

    // Sample 3: write 0.0, read -1.0
    m.inputs[0].voltage = 0.0f;
    m.process(args);
    REQUIRE_THAT(m.outputs[0].voltage, Catch::Matchers::WithinAbs(-1.0f, 1e-6f));
}

TEST_CASE("ZDelayModule: prepare resets register to zero") {
    ZDelayModule    m;
    GridProcessArgs args = make_args();
    m.prepare(args);

    // Prime the register with a nonzero value.
    m.inputs[0].voltage = 0.9f;
    m.process(args);
    m.inputs[0].voltage = 0.0f;
    m.process(args);
    REQUIRE_THAT(m.outputs[0].voltage, Catch::Matchers::WithinAbs(0.9f, 1e-6f));

    // prepare() should clear back to zero.
    m.prepare(args);
    m.inputs[0].voltage = 0.2f;
    m.process(args);
    REQUIRE_THAT(m.outputs[0].voltage, Catch::Matchers::WithinAbs(0.0f, 1e-9f));
}

TEST_CASE("ZDelayModule: zero input always produces zero output after prepare") {
    ZDelayModule    m;
    GridProcessArgs args = make_args();
    m.prepare(args);

    for (int i = 0; i < 8; ++i) {
        m.inputs[0].voltage = 0.f;
        m.process(args);
        REQUIRE_THAT(m.outputs[0].voltage, Catch::Matchers::WithinAbs(0.f, 1e-9f));
    }
}

// ---------------------------------------------------------------------------
// Engine integration
// ---------------------------------------------------------------------------

TEST_CASE("ZDelayModule: survives engine build (no param ports, no taps)") {
    GridGraph g;
    g.add_module(std::make_unique<ZDelayModule>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);
    REQUIRE(res->port_schema().size() == 0);
    REQUIRE(res->tap_schema().size() == 0);
}
