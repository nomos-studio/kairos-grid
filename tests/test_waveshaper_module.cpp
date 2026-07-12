// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for WaveshaperModule — scalar ADAA waveshaper, four shapes.

#include <kairos_grid/grid_engine.hpp>
#include <kairos_grid/grid_graph.hpp>
#include <kairos_grid/mi/waveshaper_module.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace kairos_grid;
using namespace kairos_grid::mi;

TEST_CASE("WaveshaperModule: constructs with 3 inputs and 2 outputs") {
    WaveshaperModule m(&WaveshaperModule::f_hard, &WaveshaperModule::F_hard);
    REQUIRE(m.inputs.size() == 3);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("WaveshaperModule: zero input gives zero output for all shapes") {
    using Fn                 = WaveshaperModule::ShapeFn;
    using ShapePair          = std::pair<Fn, Fn>;
    const ShapePair shapes[] = {
        {&WaveshaperModule::f_hard, &WaveshaperModule::F_hard},
        {&WaveshaperModule::f_tanh, &WaveshaperModule::F_tanh},
        {&WaveshaperModule::f_soft, &WaveshaperModule::F_soft},
        {&WaveshaperModule::f_fold, &WaveshaperModule::F_fold},
    };
    for (const auto& [f, F] : shapes) {
        auto      mod_ptr = std::make_unique<WaveshaperModule>(f, F);
        auto*     mod     = mod_ptr.get();
        GridGraph g;
        g.add_module(std::move(mod_ptr));
        auto res = g.build();
        REQUIRE(res.has_value());
        res->prepare(48000.f);

        // xl=0, prev=0: |dx| < 1e-5 → epsilon fallback → f(0)=0 for all shapes
        mod->inputs[0].voltage = 0.f;
        mod->inputs[1].voltage = 0.f;
        mod->inputs[2].voltage = 0.f;
        res->step_block(1);

        REQUIRE_THAT(mod->outputs[0].voltage, Catch::Matchers::WithinAbs(0.f, 1e-6f));
        REQUIRE_THAT(mod->outputs[1].voltage, Catch::Matchers::WithinAbs(0.f, 1e-6f));
    }
}

TEST_CASE("WaveshaperModule: hard clip at steady state uses epsilon fallback to f(x)") {
    // Once x[n] == x[n-1], |dx| < 1e-5 and ADAA falls back to f(x[n]).
    // For hard clip with |x| < 1, f(x) = x, so output exactly tracks input.
    auto mod_ptr =
        std::make_unique<WaveshaperModule>(&WaveshaperModule::f_hard, &WaveshaperModule::F_hard);
    auto*     mod = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 0.6f;
    mod->inputs[2].voltage = 0.f; // drive=0 → gain=1
    res->step_block(100);         // drive to steady state (prev converges to 0.6)

    res->step_block(1);
    REQUIRE_THAT(mod->outputs[0].voltage, Catch::Matchers::WithinAbs(0.6f, 1e-5f));
}

TEST_CASE("WaveshaperModule: hard clip ADAA output bounded to ±1 at high drive") {
    // drive=1 → gain=16; input×16 exceeds the clip threshold.
    // The ADAA output is the mean of f over [prev, x].  Since f is bounded ±1,
    // the mean is also bounded ±1.
    auto mod_ptr =
        std::make_unique<WaveshaperModule>(&WaveshaperModule::f_hard, &WaveshaperModule::F_hard);
    auto*     mod = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[2].voltage = 1.f; // drive=1 → 16× pre-gain

    const float inputs[] = {0.05f, 0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f};
    for (float v : inputs) {
        mod->inputs[0].voltage = v;
        mod->inputs[1].voltage = v;
        res->step_block(1);
        REQUIRE(std::abs(mod->outputs[0].voltage) <= 1.001f);
        REQUIRE(std::abs(mod->outputs[1].voltage) <= 1.001f);
    }
}

TEST_CASE("WaveshaperModule: tanh output bounded to ±1 at high drive") {
    auto mod_ptr =
        std::make_unique<WaveshaperModule>(&WaveshaperModule::f_tanh, &WaveshaperModule::F_tanh);
    auto*     mod = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[2].voltage = 1.f;

    const float inputs[] = {0.05f, 0.1f, -0.1f, 0.2f, -0.2f, 0.3f};
    for (float v : inputs) {
        mod->inputs[0].voltage = v;
        mod->inputs[1].voltage = -v;
        res->step_block(1);
        REQUIRE(std::abs(mod->outputs[0].voltage) <= 1.001f);
        REQUIRE(std::abs(mod->outputs[1].voltage) <= 1.001f);
    }
}

TEST_CASE("WaveshaperModule: sine fold output bounded to ±1") {
    // f_fold(x) = sin(π/2·x) is bounded ±1; ADAA mean is also bounded ±1.
    auto mod_ptr =
        std::make_unique<WaveshaperModule>(&WaveshaperModule::f_fold, &WaveshaperModule::F_fold);
    auto*     mod = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[2].voltage = 1.f; // 16× gain → multiple fold periods

    const float inputs[] = {0.05f, 0.1f, 0.15f, -0.1f, -0.2f, 0.3f};
    for (float v : inputs) {
        mod->inputs[0].voltage = v;
        res->step_block(1);
        REQUIRE(std::abs(mod->outputs[0].voltage) <= 1.001f);
    }
}

TEST_CASE("WaveshaperModule: prepare() resets ADAA state to zero") {
    // Run to steady state at x=0.5 (epsilon fallback: output = f(0.5) = 0.5).
    // Call prepare() to reset prev to 0.
    // First new sample at x=0.5: ADAA(0.5, 0) = (F(0.5)-F(0))/0.5 = 0.125/0.5 = 0.25.
    // The two values (0.5 vs 0.25) are distinguishably different.
    auto mod_ptr =
        std::make_unique<WaveshaperModule>(&WaveshaperModule::f_hard, &WaveshaperModule::F_hard);
    auto*     mod = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 0.5f;
    mod->inputs[2].voltage = 0.f;
    res->step_block(100); // settle: prev → 0.5

    res->prepare(48000.f); // reset prev to 0
    res->step_block(1);
    // ADAA(0.5, 0) = (0.5²/2) / 0.5 = 0.25, not the steady-state f(0.5)=0.5
    REQUIRE_THAT(mod->outputs[0].voltage, Catch::Matchers::WithinAbs(0.25f, 1e-4f));
}

TEST_CASE("WaveshaperModule: stereo channels processed independently") {
    auto mod_ptr =
        std::make_unique<WaveshaperModule>(&WaveshaperModule::f_hard, &WaveshaperModule::F_hard);
    auto*     mod = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 0.4f; // L
    mod->inputs[1].voltage = 0.8f; // R — different
    mod->inputs[2].voltage = 0.f;
    res->step_block(100); // drive to stereo steady states

    // At steady state: output = f(x) for each channel independently
    res->step_block(1);
    REQUIRE_THAT(mod->outputs[0].voltage, Catch::Matchers::WithinAbs(0.4f, 1e-5f));
    REQUIRE_THAT(mod->outputs[1].voltage, Catch::Matchers::WithinAbs(0.8f, 1e-5f));
}

// ---------------------------------------------------------------------------
// Triangular wavefolder (f_fold_tri / F_fold_tri)
// ---------------------------------------------------------------------------

TEST_CASE("WaveshaperModule: triangular fold — output bounded ±1 at high drive") {
    // At high drive (16×) the signal sweeps through many fold periods.
    // The ADAA mean-value formula averages over all folds; f_fold_tri ∈ [−1,1]
    // everywhere, so the ADAA output must also be in [−1,1].
    auto      mod_ptr = std::make_unique<WaveshaperModule>(&WaveshaperModule::f_fold_tri,
                                                           &WaveshaperModule::F_fold_tri);
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[2].voltage = 1.f; // drive=1 → 16× pre-gain

    const float inputs[] = {0.05f, 0.1f, 0.15f, -0.1f, -0.2f, 0.3f, -0.3f};
    for (float v : inputs) {
        mod->inputs[0].voltage = v;
        mod->inputs[1].voltage = -v;
        res->step_block(1);
        REQUIRE(std::abs(mod->outputs[0].voltage) <= 1.001f);
        REQUIRE(std::abs(mod->outputs[1].voltage) <= 1.001f);
    }
}

TEST_CASE("WaveshaperModule: triangular fold — steady-state epsilon fallback") {
    // At steady state (same sample repeated), ADAA falls back to f(x).
    // For x=0.5 (well within a fold segment), f_fold_tri(0.5)=0.5 (slope +1, xr=0.5).
    auto      mod_ptr = std::make_unique<WaveshaperModule>(&WaveshaperModule::f_fold_tri,
                                                           &WaveshaperModule::F_fold_tri);
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 0.5f;
    mod->inputs[2].voltage = 0.f; // drive=0 → gain=1
    res->step_block(100);         // settle

    res->step_block(1);
    REQUIRE_THAT(mod->outputs[0].voltage, Catch::Matchers::WithinAbs(0.5f, 1e-5f));
}

TEST_CASE("WaveshaperModule: triangular fold — antiderivative continuity at fold point") {
    // ADAA(prev, x) = (F(x) − F(prev)) / (x − prev).
    // Cross the fold point at x=1 (segment 0→1 boundary): prev=0.5, x=1.5.
    // f_fold_tri on [0.5, 1.5] rises 0→1 then falls 1→0; mean = 0.5.
    // F(1.5) = F_fold_tri(1.5): n=round(0.75)=1, xr=−0.5, q=0.125 → F=0.875
    // F(0.5) = F_fold_tri(0.5): n=round(0.25)=0, xr=0.5, q=0.125  → F=0.125
    // ADAA = (0.875 − 0.125) / (1.5 − 0.5) = 0.75 / 1.0 = 0.75 … wait
    // Actually mean of rising-then-falling triangle over [0.5,1.5]: area = 0.75, len=1 → 0.75.
    auto      mod_ptr = std::make_unique<WaveshaperModule>(&WaveshaperModule::f_fold_tri,
                                                           &WaveshaperModule::F_fold_tri);
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);
    mod->inputs[2].voltage = 0.f; // no drive taper — test raw ADAA values

    // Step 1: prev=0, x=0.5 (ADAA from 0 to 0.5 = mean of triangle = 0.25)
    mod->inputs[0].voltage = 0.5f;
    res->step_block(1);
    // F(0.5)=0.125, F(0)=0, dx=0.5 → 0.125/0.5 = 0.25
    REQUIRE_THAT(mod->outputs[0].voltage, Catch::Matchers::WithinAbs(0.25f, 1e-4f));

    // Step 2: prev=0.5, x=1.5 — crosses the fold point at x=1
    mod->inputs[0].voltage = 1.5f;
    res->step_block(1);
    // F(1.5)=0.875, F(0.5)=0.125, dx=1.0 → 0.75/1.0 = 0.75
    REQUIRE_THAT(mod->outputs[0].voltage, Catch::Matchers::WithinAbs(0.75f, 1e-4f));
}
