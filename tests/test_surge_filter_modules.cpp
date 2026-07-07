// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for Surge XT sst-filters module wrappers.

#include <kairos_grid/grid_engine.hpp>
#include <kairos_grid/grid_graph.hpp>
#include <kairos_grid/surge/surge_filter_module.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace kairos_grid;
using namespace kairos_grid::surge;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static GridEngine build_filter(std::unique_ptr<GridModule> m, float sr = 48000.f) {
    GridGraph g;
    g.add_module(std::move(m));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(sr);
    return std::move(*res);
}

// ---------------------------------------------------------------------------
// VintageLadderModule
// ---------------------------------------------------------------------------

TEST_CASE("VintageLadderModule: constructs with 4 inputs and 2 outputs") {
    VintageLadderModule m;
    REQUIRE(m.inputs.size() == 4);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("VintageLadderModule: passes DC through at reso=0, cutoff=127") {
    auto  mod_ptr = std::make_unique<VintageLadderModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 5.f; // full-scale L
    mod->inputs[1].voltage = 0.f;
    mod->inputs[2].voltage = 127.f; // cutoff wide open
    mod->inputs[3].voltage = 0.f;   // zero resonance

    // Warm-up: enough for coefficient ramp to settle.
    for (int i = 0; i < 256; ++i) {
        res->step_block(1);
    }

    // With cutoff fully open and zero resonance the filter is nearly unity.
    REQUIRE(std::isfinite(mod->outputs[0].voltage));
    REQUIRE(std::abs(mod->outputs[0].voltage) > 0.5f); // significant pass-through
}

TEST_CASE("VintageLadderModule: attenuates at low cutoff") {
    auto  mod_ptr = std::make_unique<VintageLadderModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // Drive with full-scale signal, very low cutoff.
    mod->inputs[0].voltage = 5.f;
    mod->inputs[1].voltage = 0.f;
    mod->inputs[2].voltage = 20.f; // ~27 Hz
    mod->inputs[3].voltage = 0.f;

    for (int i = 0; i < 2048; ++i)
        res->step_block(1);

    // Finite output and significantly attenuated vs input.
    REQUIRE(std::isfinite(mod->outputs[0].voltage));
    REQUIRE(std::abs(mod->outputs[0].voltage) < 4.f);
}

TEST_CASE("VintageLadderModule: output bounded after sustained signal") {
    auto  mod_ptr = std::make_unique<VintageLadderModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[1].voltage = 1.f;
    mod->inputs[2].voltage = 69.f; // A4
    mod->inputs[3].voltage = 0.5f;

    bool finite  = true;
    bool bounded = true;
    for (int i = 0; i < 9600; ++i) {
        res->step_block(1);
        const float L = mod->outputs[0].voltage;
        const float R = mod->outputs[1].voltage;
        if (!std::isfinite(L) || !std::isfinite(R)) {
            finite = false;
            break;
        }
        if (std::abs(L) > 6.f || std::abs(R) > 6.f) {
            bounded = false;
            break;
        }
    }
    REQUIRE(finite);
    REQUIRE(bounded);
}

// ---------------------------------------------------------------------------
// DiodeLadderModule
// ---------------------------------------------------------------------------

TEST_CASE("DiodeLadderModule: constructs correctly") {
    DiodeLadderModule m;
    REQUIRE(m.inputs.size() == 4);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("DiodeLadderModule: output is finite after warm-up") {
    auto  mod_ptr = std::make_unique<DiodeLadderModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 2.f;
    mod->inputs[1].voltage = 2.f;
    mod->inputs[2].voltage = 60.f; // C4
    mod->inputs[3].voltage = 0.3f;

    res->step_block(4800);

    REQUIRE(std::isfinite(mod->outputs[0].voltage));
    REQUIRE(std::isfinite(mod->outputs[1].voltage));
}

// ---------------------------------------------------------------------------
// K35 filters
// ---------------------------------------------------------------------------

TEST_CASE("K35LPModule: output is finite and bounded") {
    auto  mod_ptr = std::make_unique<K35LPModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[1].voltage = 1.f;
    mod->inputs[2].voltage = 72.f;
    mod->inputs[3].voltage = 0.4f;

    bool ok = true;
    for (int i = 0; i < 9600; ++i) {
        res->step_block(1);
        if (!std::isfinite(mod->outputs[0].voltage) || std::abs(mod->outputs[0].voltage) > 8.f) {
            ok = false;
            break;
        }
    }
    REQUIRE(ok);
}

TEST_CASE("K35HPModule: output is finite and bounded") {
    auto  mod_ptr = std::make_unique<K35HPModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[1].voltage = 1.f;
    mod->inputs[2].voltage = 60.f;
    mod->inputs[3].voltage = 0.4f;

    bool ok = true;
    for (int i = 0; i < 9600; ++i) {
        res->step_block(1);
        if (!std::isfinite(mod->outputs[0].voltage) || std::abs(mod->outputs[0].voltage) > 8.f) {
            ok = false;
            break;
        }
    }
    REQUIRE(ok);
}

// ---------------------------------------------------------------------------
// OBXD 4-pole
// ---------------------------------------------------------------------------

TEST_CASE("OBXD4PoleModule: output is finite and bounded") {
    auto  mod_ptr = std::make_unique<OBXD4PoleModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[1].voltage = 1.f;
    mod->inputs[2].voltage = 69.f;
    mod->inputs[3].voltage = 0.5f;

    bool ok = true;
    for (int i = 0; i < 9600; ++i) {
        res->step_block(1);
        if (!std::isfinite(mod->outputs[0].voltage) || std::abs(mod->outputs[0].voltage) > 8.f) {
            ok = false;
            break;
        }
    }
    REQUIRE(ok);
}

// ---------------------------------------------------------------------------
// Biquad filters (LP12, LP24, HP12, HP24, BP12, BP24)
// ---------------------------------------------------------------------------

TEST_CASE("LP12Module: output is finite and bounded") {
    auto  mod_ptr = std::make_unique<LP12Module>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[2].voltage = 60.f;
    mod->inputs[3].voltage = 0.5f;

    bool ok = true;
    for (int i = 0; i < 4800; ++i) {
        res->step_block(1);
        if (!std::isfinite(mod->outputs[0].voltage) || std::abs(mod->outputs[0].voltage) > 8.f) {
            ok = false;
            break;
        }
    }
    REQUIRE(ok);
}

TEST_CASE("HP24Module: output is finite and bounded") {
    auto  mod_ptr = std::make_unique<HP24Module>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[2].voltage = 48.f; // C3
    mod->inputs[3].voltage = 0.3f;

    bool ok = true;
    for (int i = 0; i < 4800; ++i) {
        res->step_block(1);
        if (!std::isfinite(mod->outputs[0].voltage) || std::abs(mod->outputs[0].voltage) > 8.f) {
            ok = false;
            break;
        }
    }
    REQUIRE(ok);
}

// ---------------------------------------------------------------------------
// Integration: oscillator + filter chain (no oscillator headers here — use
// a raw DC signal through a filter to verify the graph plumbing.)
// ---------------------------------------------------------------------------

TEST_CASE("Filter modules: two different filters in parallel graph (no crash)") {
    GridGraph g;
    int       i_lp = g.add_module(std::make_unique<VintageLadderModule>());
    int       i_hp = g.add_module(std::make_unique<K35HPModule>());

    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    auto* lp = dynamic_cast<VintageLadderModule*>(res->module(i_lp));
    REQUIRE(lp != nullptr);
    lp->inputs[0].voltage = 3.f;
    lp->inputs[2].voltage = 69.f;
    lp->inputs[3].voltage = 0.5f;

    auto* hp = dynamic_cast<K35HPModule*>(res->module(i_hp));
    REQUIRE(hp != nullptr);
    hp->inputs[0].voltage = 3.f;
    hp->inputs[2].voltage = 57.f;
    hp->inputs[3].voltage = 0.3f;

    res->step_block(4800);

    REQUIRE(std::isfinite(lp->outputs[0].voltage));
    REQUIRE(std::isfinite(hp->outputs[0].voltage));
}
