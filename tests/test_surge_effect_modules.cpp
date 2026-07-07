// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for Surge XT Effect module wrappers.

#include <kairos_grid/grid_engine.hpp>
#include <kairos_grid/grid_graph.hpp>
#include <kairos_grid/surge/surge_effect_module.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace kairos_grid;
using namespace kairos_grid::surge;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

template <typename ModT> static GridEngine build_effect(std::unique_ptr<ModT> m) {
    GridGraph g;
    g.add_module(std::move(m));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);
    return std::move(*res);
}

static bool output_bounded_and_finite(GridEngine& eng, GridModule* mod, int n_samples = 9600,
                                      float bound = 10.f) {
    for (int i = 0; i < n_samples; ++i) {
        eng.step_block(1);
        const float L = mod->outputs[0].voltage;
        const float R = mod->outputs[1].voltage;
        if (!std::isfinite(L) || !std::isfinite(R))
            return false;
        if (std::abs(L) > bound || std::abs(R) > bound)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Port count checks
// ---------------------------------------------------------------------------

TEST_CASE("Reverb1Module: correct port counts") {
    Reverb1Module m;
    REQUIRE(m.inputs.size() == 14); // 2 audio + 12 params
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("Reverb2Module: correct port counts") {
    Reverb2Module m;
    REQUIRE(m.inputs.size() == 14);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("ChorusModule: correct port counts") {
    ChorusModule m;
    REQUIRE(m.inputs.size() == 14);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("DelayModule: correct port counts") {
    DelayModule m;
    REQUIRE(m.inputs.size() == 14);
    REQUIRE(m.outputs.size() == 2);
}

// ---------------------------------------------------------------------------
// Reverb 1 — output is finite and bounded at default parameters
// ---------------------------------------------------------------------------

TEST_CASE("Reverb1Module: output is finite and bounded with impulse input") {
    auto  mod_ptr = std::make_unique<Reverb1Module>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // Single-sample impulse through the reverb.
    mod->inputs[0].voltage = 5.f;
    mod->inputs[1].voltage = 5.f;
    res->step_block(1);
    mod->inputs[0].voltage = 0.f;
    mod->inputs[1].voltage = 0.f;

    REQUIRE(output_bounded_and_finite(*res, mod, 48000, 10.f));
}

TEST_CASE("Reverb1Module: reverb tail is non-zero after impulse") {
    auto  mod_ptr = std::make_unique<Reverb1Module>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // Drive with 32 samples of full-scale input (one reverb block).
    mod->inputs[0].voltage = 5.f;
    mod->inputs[1].voltage = 5.f;
    res->step_block(64); // two blocks to ensure effect has seen input
    mod->inputs[0].voltage = 0.f;
    mod->inputs[1].voltage = 0.f;

    // Reverb tail should produce non-zero output for a while.
    bool any_output = false;
    for (int i = 0; i < 4800; ++i) {
        res->step_block(1);
        if (std::abs(mod->outputs[0].voltage) > 1e-4f ||
            std::abs(mod->outputs[1].voltage) > 1e-4f) {
            any_output = true;
            break;
        }
    }
    REQUIRE(any_output);
}

// ---------------------------------------------------------------------------
// Reverb 2
// ---------------------------------------------------------------------------

TEST_CASE("Reverb2Module: output is finite and bounded") {
    auto  mod_ptr = std::make_unique<Reverb2Module>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[1].voltage = 1.f;

    REQUIRE(output_bounded_and_finite(*res, mod, 9600, 10.f));
}

// ---------------------------------------------------------------------------
// Chorus
// ---------------------------------------------------------------------------

TEST_CASE("ChorusModule: output is finite and bounded") {
    auto  mod_ptr = std::make_unique<ChorusModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[1].voltage = 1.f;

    REQUIRE(output_bounded_and_finite(*res, mod));
}

TEST_CASE("ChorusModule: produces output when driven with signal") {
    auto  mod_ptr = std::make_unique<ChorusModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 3.f;
    mod->inputs[1].voltage = 3.f;

    res->step_block(4800);
    REQUIRE(std::isfinite(mod->outputs[0].voltage));
    REQUIRE(std::isfinite(mod->outputs[1].voltage));
    // Chorus with mix != 0 should produce signal.
    const bool any =
        std::abs(mod->outputs[0].voltage) > 1e-4f || std::abs(mod->outputs[1].voltage) > 1e-4f;
    REQUIRE(any);
}

// ---------------------------------------------------------------------------
// Delay
// ---------------------------------------------------------------------------

TEST_CASE("DelayModule: output is finite and bounded") {
    auto  mod_ptr = std::make_unique<DelayModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[1].voltage = 1.f;

    REQUIRE(output_bounded_and_finite(*res, mod));
}

// ---------------------------------------------------------------------------
// Phaser
// ---------------------------------------------------------------------------

TEST_CASE("PhaserModule: output is finite and bounded") {
    auto  mod_ptr = std::make_unique<PhaserModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[1].voltage = 1.f;

    REQUIRE(output_bounded_and_finite(*res, mod));
}

// ---------------------------------------------------------------------------
// Flanger
// ---------------------------------------------------------------------------

TEST_CASE("FlangerModule: output is finite and bounded") {
    auto  mod_ptr = std::make_unique<FlangerModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[1].voltage = 1.f;

    REQUIRE(output_bounded_and_finite(*res, mod));
}

// ---------------------------------------------------------------------------
// Bonsai (tape saturation / character)
// ---------------------------------------------------------------------------

TEST_CASE("BonsaiModule: output is finite and bounded") {
    auto  mod_ptr = std::make_unique<BonsaiModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[1].voltage = 1.f;

    REQUIRE(output_bounded_and_finite(*res, mod));
}

// ---------------------------------------------------------------------------
// Ensemble (BBD chorus)
// ---------------------------------------------------------------------------

TEST_CASE("EnsembleModule: output is finite and bounded") {
    auto  mod_ptr = std::make_unique<EnsembleModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 1.f;
    mod->inputs[1].voltage = 1.f;

    REQUIRE(output_bounded_and_finite(*res, mod));
}

// ---------------------------------------------------------------------------
// Integration: two effects in parallel (no crash)
// ---------------------------------------------------------------------------

TEST_CASE("Effect modules: Reverb2 + Flanger in parallel graph (no crash)") {
    GridGraph g;
    int       i_rev = g.add_module(std::make_unique<Reverb2Module>());
    int       i_fla = g.add_module(std::make_unique<FlangerModule>());

    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    auto* rev = dynamic_cast<Reverb2Module*>(res->module(i_rev));
    REQUIRE(rev != nullptr);
    rev->inputs[0].voltage = 2.f;
    rev->inputs[1].voltage = 2.f;

    auto* fla = dynamic_cast<FlangerModule*>(res->module(i_fla));
    REQUIRE(fla != nullptr);
    fla->inputs[0].voltage = 2.f;
    fla->inputs[1].voltage = 2.f;

    res->step_block(4800);

    REQUIRE(std::isfinite(rev->outputs[0].voltage));
    REQUIRE(std::isfinite(fla->outputs[0].voltage));
}
