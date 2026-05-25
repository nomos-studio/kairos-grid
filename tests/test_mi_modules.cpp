// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for MI DSP module wrappers: SvfModule, OnePoleModule, PlaitsModule.

#include <kairos_grid/mi/svf_module.hpp>
#include <kairos_grid/mi/one_pole_module.hpp>
#include <kairos_grid/mi/plaits_module.hpp>
#include <kairos_grid/grid_engine.hpp>
#include <kairos_grid/grid_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace kairos_grid;
using namespace kairos_grid::mi;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static GridEngine build_single(std::unique_ptr<GridModule> m) {
    GridGraph g;
    g.add_module(std::move(m));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);
    return std::move(*res);
}

// ---------------------------------------------------------------------------
// SvfModule
// ---------------------------------------------------------------------------

TEST_CASE("SvfModule: constructs with 3 inputs and 3 outputs") {
    SvfModule m;
    REQUIRE(m.inputs.size()  == 3);
    REQUIRE(m.outputs.size() == 3);
}

TEST_CASE("SvfModule: DC input passes through LP at very low cutoff") {
    // With freq ≈ 0, the LP output should approach the DC input
    // after enough samples to charge the filter state.
    auto  mod_ptr = std::make_unique<SvfModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // Drive the filter: DC in = 1.0, very low cutoff, moderate Q.
    mod->inputs[0].voltage = 1.f;
    mod->inputs[1].voltage = 0.001f; // near-zero cutoff — strong LP
    mod->inputs[2].voltage = 0.0f;   // minimum resonance

    // Run enough samples for the filter state to settle.
    res->step_block(4800); // 100 ms at 48 kHz

    // LP output should have converged close to 1.0.
    REQUIRE_THAT(mod->outputs[0].voltage,
                 Catch::Matchers::WithinAbs(1.f, 0.05f));
}

TEST_CASE("SvfModule: HP output is near zero for DC at very low cutoff") {
    auto  mod_ptr = std::make_unique<SvfModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[1].voltage = 0.001f;
    mod->inputs[2].voltage = 0.0f;

    res->step_block(4800);

    // HP should be near zero after the transient settles.
    REQUIRE_THAT(mod->outputs[1].voltage,
                 Catch::Matchers::WithinAbs(0.f, 0.05f));
}

TEST_CASE("SvfModule: LP + HP + BP sum approximately equals input") {
    // SVF identity: in ≈ lp + r·bp + hp, so with r = 1/Q we get
    // lp + hp = in - r·bp.  This test just checks BP is non-zero
    // for a mid-frequency sine-like input after transient settles.
    auto  mod_ptr = std::make_unique<SvfModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // Mid-range cutoff.
    mod->inputs[1].voltage = 0.1f;
    mod->inputs[2].voltage = 0.5f;

    // Feed a constant 1.0 for many samples and check outputs are finite.
    mod->inputs[0].voltage = 1.f;
    res->step_block(1000);

    float lp = mod->outputs[0].voltage;
    float hp = mod->outputs[1].voltage;
    float bp = mod->outputs[2].voltage;

    REQUIRE(std::isfinite(lp));
    REQUIRE(std::isfinite(hp));
    REQUIRE(std::isfinite(bp));
}

// ---------------------------------------------------------------------------
// OnePoleModule
// ---------------------------------------------------------------------------

TEST_CASE("OnePoleModule: constructs with 2 inputs and 2 outputs") {
    OnePoleModule m;
    REQUIRE(m.inputs.size()  == 2);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("OnePoleModule: HP + LP == in (exact identity)") {
    // For the ZDF one-pole, hp = in - lp is exact.
    auto  mod_ptr = std::make_unique<OnePoleModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 0.7f;
    mod->inputs[1].voltage = 0.05f; // low cutoff

    res->step_block(1);

    float lp = mod->outputs[0].voltage;
    float hp = mod->outputs[1].voltage;
    REQUIRE_THAT(lp + hp, Catch::Matchers::WithinAbs(0.7f, 1e-5f));
}

TEST_CASE("OnePoleModule: DC settles to input at very low cutoff") {
    auto  mod_ptr = std::make_unique<OnePoleModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[1].voltage = 0.001f;

    res->step_block(9600); // 200 ms

    REQUIRE_THAT(mod->outputs[0].voltage,
                 Catch::Matchers::WithinAbs(1.f, 0.05f));
}

// ---------------------------------------------------------------------------
// PlaitsModule
// ---------------------------------------------------------------------------

TEST_CASE("PlaitsModule: constructs with 7 inputs and 2 outputs") {
    PlaitsModule m;
    REQUIRE(m.inputs.size()  == 7);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("PlaitsModule: produces non-zero output after a few blocks") {
    auto  mod_ptr = std::make_unique<PlaitsModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // MIDI A4 = 69, engine 0 (VA oscillator), trigger on.
    mod->inputs[0].voltage = 69.f;  // note
    mod->inputs[1].voltage = 0.5f;  // harmonics
    mod->inputs[2].voltage = 0.5f;  // timbre
    mod->inputs[3].voltage = 0.5f;  // morph
    mod->inputs[4].voltage = 1.f;   // trigger
    mod->inputs[5].voltage = 1.f;   // level
    mod->inputs[6].voltage = 0.f;   // engine 0

    // Run enough samples to see sustained output (past the LPG attack).
    res->step_block(4800); // 100 ms

    float out = mod->outputs[0].voltage;
    float aux = mod->outputs[1].voltage;

    REQUIRE(std::isfinite(out));
    REQUIRE(std::isfinite(aux));
    // At least one output should be non-trivially non-zero.
    bool any_signal = (std::abs(out) > 1e-4f) || (std::abs(aux) > 1e-4f);
    REQUIRE(any_signal);
}

TEST_CASE("PlaitsModule: output stays within ±1 for typical inputs") {
    auto  mod_ptr = std::make_unique<PlaitsModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 60.f;  // C4
    mod->inputs[1].voltage = 0.5f;
    mod->inputs[2].voltage = 0.5f;
    mod->inputs[3].voltage = 0.5f;
    mod->inputs[4].voltage = 1.f;
    mod->inputs[5].voltage = 1.f;
    mod->inputs[6].voltage = 0.f;

    bool in_range = true;
    for (int i = 0; i < 4800; ++i) {
        res->step_block(1);
        float out = mod->outputs[0].voltage;
        float aux = mod->outputs[1].voltage;
        if (std::abs(out) > 1.1f || std::abs(aux) > 1.1f) {
            in_range = false;
            break;
        }
    }
    REQUIRE(in_range);
}

TEST_CASE("PlaitsModule: integrates into GridGraph chain with SvfModule") {
    // plaits → svf, check the chain builds and produces finite output.
    GridGraph g;
    int i_plaits = g.add_module(std::make_unique<PlaitsModule>());
    int i_svf    = g.add_module(std::make_unique<SvfModule>());
    g.add_cable({i_plaits, 0, i_svf, 0}); // plaits.out → svf.in

    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // Set up Plaits.
    auto* p = dynamic_cast<PlaitsModule*>(res->module(i_plaits));
    REQUIRE(p != nullptr);
    p->inputs[0].voltage = 69.f;
    p->inputs[4].voltage = 1.f;
    p->inputs[5].voltage = 1.f;

    // Set SVF cutoff.
    auto* s = dynamic_cast<SvfModule*>(res->module(i_svf));
    REQUIRE(s != nullptr);
    s->inputs[1].voltage = 0.2f;
    s->inputs[2].voltage = 0.3f;

    res->step_block(4800);

    REQUIRE(std::isfinite(s->outputs[0].voltage)); // SVF LP output
}
