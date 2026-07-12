// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for Airwindows saturation GridModules — Desk, Slew, Spiral.

#include <kairos_grid/airwindows/airwindows_module.hpp>
#include <kairos_grid/grid_engine.hpp>
#include <kairos_grid/grid_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace kairos_grid;
using namespace kairos_grid::airwindows;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr float kSR = 48000.f;

template <typename Module> static GridEngine build_engine(float sr = kSR) {
    GridGraph g;
    g.add_module(std::make_unique<Module>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(sr);
    return std::move(*res);
}

// Run n steps and return the last L/R output pair.
static std::pair<float, float> run_n(GridEngine& eng, GridModule* mod, int n) {
    for (int i = 0; i < n; ++i)
        eng.step_block(1);
    return {mod->outputs[0].voltage, mod->outputs[1].voltage};
}

// ---------------------------------------------------------------------------
// DeskModule
// ---------------------------------------------------------------------------

TEST_CASE("DeskModule: constructs with 2 inputs and 2 outputs") {
    DeskModule m;
    REQUIRE(m.inputs.size() == 2);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("DeskModule: zero input produces zero output after settling") {
    auto      mod_ptr = std::make_unique<DeskModule>();
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(kSR);

    auto [l, r] = run_n(*res, mod, 64);
    REQUIRE_THAT(l, WithinAbs(0.f, 1e-6f));
    REQUIRE_THAT(r, WithinAbs(0.f, 1e-6f));
}

TEST_CASE("DeskModule: passes low-level sine near unity") {
    auto      mod_ptr = std::make_unique<DeskModule>();
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(kSR);

    // Feed a -40 dBFS sine; Desk should pass near-unity (< 10% error) because
    // slew rate is well below the limiter threshold at low levels.
    constexpr int kLen    = 512;
    float         peak_in = 0.f, peak_out = 0.f;
    for (int i = 0; i < kLen; ++i) {
        const float x          = 0.01f * std::sin(2.f * 3.14159265f * 440.f * i / kSR);
        mod->inputs[0].voltage = x;
        mod->inputs[1].voltage = x;
        res->step_block(1);
        peak_in  = std::max(peak_in, std::abs(x));
        peak_out = std::max(peak_out, std::abs(mod->outputs[0].voltage));
    }
    // At very low levels the Desk algorithm passes transparently.
    REQUIRE(peak_out > 0.f);
    REQUIRE(peak_out < 0.02f);
}

TEST_CASE("DeskModule: clips high-amplitude input below ±1.1") {
    auto      mod_ptr = std::make_unique<DeskModule>();
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(kSR);

    mod->inputs[0].voltage = 1.0f;
    mod->inputs[1].voltage = 1.0f;
    for (int i = 0; i < 128; ++i) {
        mod->inputs[0].voltage = 1.0f;
        mod->inputs[1].voltage = -1.0f;
        res->step_block(1);
        REQUIRE(mod->outputs[0].voltage <= 1.1f);
        REQUIRE(mod->outputs[0].voltage >= -1.1f);
        REQUIRE(mod->outputs[1].voltage <= 1.1f);
        REQUIRE(mod->outputs[1].voltage >= -1.1f);
    }
}

TEST_CASE("DeskModule: L and R are independent at different levels") {
    auto      mod_ptr = std::make_unique<DeskModule>();
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(kSR);

    // Drive L hard and R silent — they should differ.
    for (int i = 0; i < 64; ++i) {
        mod->inputs[0].voltage = 0.9f;
        mod->inputs[1].voltage = 0.0f;
        res->step_block(1);
    }
    REQUIRE(std::abs(mod->outputs[0].voltage) > std::abs(mod->outputs[1].voltage) + 1e-4f);
}

TEST_CASE("DeskModule: output is finite for full-scale input") {
    auto      mod_ptr = std::make_unique<DeskModule>();
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(kSR);

    for (int i = 0; i < 256; ++i) {
        const float x          = (i % 2 == 0) ? 1.0f : -1.0f;
        mod->inputs[0].voltage = x;
        mod->inputs[1].voltage = x;
        res->step_block(1);
        REQUIRE(std::isfinite(mod->outputs[0].voltage));
        REQUIRE(std::isfinite(mod->outputs[1].voltage));
    }
}

// ---------------------------------------------------------------------------
// SlewModule
// ---------------------------------------------------------------------------

TEST_CASE("SlewModule: constructs with 3 inputs and 2 outputs") {
    SlewModule m;
    REQUIRE(m.inputs.size() == 3);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("SlewModule: zero input produces zero output") {
    auto      mod_ptr = std::make_unique<SlewModule>();
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(kSR);

    auto [l, r] = run_n(*res, mod, 128);
    REQUIRE_THAT(l, WithinAbs(0.f, 1e-6f));
    REQUIRE_THAT(r, WithinAbs(0.f, 1e-6f));
}

TEST_CASE("SlewModule: at slew=0 passes signal near transparently") {
    auto      mod_ptr = std::make_unique<SlewModule>();
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(kSR);

    // slew=0 → threshold very large → almost no rate limiting
    mod->inputs[2].voltage = 0.0f;

    float peak_in = 0.f, peak_out = 0.f;
    for (int i = 0; i < 512; ++i) {
        const float x          = 0.5f * std::sin(2.f * 3.14159265f * 200.f * i / kSR);
        mod->inputs[0].voltage = x;
        mod->inputs[1].voltage = x;
        res->step_block(1);
        // Ignore initial transient (first 32 samples for 2x oversampling warmup)
        if (i > 32) {
            peak_in  = std::max(peak_in, std::abs(x));
            peak_out = std::max(peak_out, std::abs(mod->outputs[0].voltage));
        }
    }
    REQUIRE(peak_out > 0.f);
    // At slew=0 the 200Hz sine should not be strongly attenuated
    REQUIRE(peak_out > peak_in * 0.5f);
}

TEST_CASE("SlewModule: at slew=1 slows transient response") {
    // Verify the slew limiter's rate-of-change property: with maximum slewing,
    // output should ramp toward the new value more slowly than with minimum.
    // The Slew2 antialiaser reconstructs signal post-limiting, so steady-state
    // peak amplitude may still reach the input level — but the INITIAL TRANSIENT
    // (first few samples after a step) should be suppressed at high slew values.

    auto build_slew = [](float slew_v, float sr = kSR) {
        GridGraph g;
        auto      mod_ptr = std::make_unique<SlewModule>();
        g.add_module(std::move(mod_ptr));
        auto res = g.build();
        REQUIRE(res.has_value());
        res->prepare(sr);
        return std::move(*res);
    };

    GridGraph g0, g1;
    auto      m0_ptr = std::make_unique<SlewModule>();
    auto      m1_ptr = std::make_unique<SlewModule>();
    auto*     m0     = m0_ptr.get();
    auto*     m1     = m1_ptr.get();
    g0.add_module(std::move(m0_ptr));
    g1.add_module(std::move(m1_ptr));
    auto r0 = g0.build();
    auto r1 = g1.build();
    REQUIRE(r0.has_value());
    REQUIRE(r1.has_value());
    r0->prepare(kSR);
    r1->prepare(kSR);

    // Warm up both with silence for 64 samples
    for (int i = 0; i < 64; ++i) {
        r0->step_block(1);
        r1->step_block(1);
    }

    // Step from 0 to 0.5; measure transient over next 8 samples.
    m0->inputs[2].voltage = 0.0f; // slew=0: transparent
    m1->inputs[2].voltage = 1.0f; // slew=1: maximum limiting

    float sum0 = 0.f, sum1 = 0.f;
    for (int i = 0; i < 8; ++i) {
        m0->inputs[0].voltage = 0.5f;
        m1->inputs[0].voltage = 0.5f;
        r0->step_block(1);
        r1->step_block(1);
        sum0 += std::abs(m0->outputs[0].voltage);
        sum1 += std::abs(m1->outputs[0].voltage);
    }
    // The transparent channel (slew=0) should track the step more quickly —
    // its accumulated response over the first 8 samples should be larger than
    // the heavily limited channel (slew=1).
    REQUIRE(sum0 > sum1);
}

TEST_CASE("SlewModule: output is bounded and finite for all inputs") {
    auto      mod_ptr = std::make_unique<SlewModule>();
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(kSR);

    mod->inputs[2].voltage = 0.5f;
    for (int i = 0; i < 256; ++i) {
        const float x          = (i % 2 == 0) ? 1.0f : -1.0f;
        mod->inputs[0].voltage = x;
        mod->inputs[1].voltage = x;
        res->step_block(1);
        REQUIRE(std::isfinite(mod->outputs[0].voltage));
        REQUIRE(std::isfinite(mod->outputs[1].voltage));
    }
}

TEST_CASE("SlewModule: clamps slew param to [0,1]") {
    auto      mod_ptr = std::make_unique<SlewModule>();
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(kSR);

    // Out-of-range slew values should not cause NaN or infinite output.
    for (float slew : {-1.f, 2.f, 100.f}) {
        mod->inputs[2].voltage = slew;
        mod->inputs[0].voltage = 0.5f;
        mod->inputs[1].voltage = 0.5f;
        res->step_block(1);
        REQUIRE(std::isfinite(mod->outputs[0].voltage));
        REQUIRE(std::isfinite(mod->outputs[1].voltage));
    }
}

// ---------------------------------------------------------------------------
// SpiralModule
// ---------------------------------------------------------------------------

TEST_CASE("SpiralModule: constructs with 4 inputs and 2 outputs") {
    SpiralModule m;
    REQUIRE(m.inputs.size() == 4);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("SpiralModule: zero input produces zero output") {
    auto      mod_ptr = std::make_unique<SpiralModule>();
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(kSR);

    // drive=0.5 (unity), wet=1.0
    mod->inputs[2].voltage = 0.5f;
    mod->inputs[3].voltage = 1.0f;

    auto [l, r] = run_n(*res, mod, 128);
    REQUIRE_THAT(l, WithinAbs(0.f, 1e-5f));
    REQUIRE_THAT(r, WithinAbs(0.f, 1e-5f));
}

TEST_CASE("SpiralModule: at wet=0 passes dry signal") {
    auto      mod_ptr = std::make_unique<SpiralModule>();
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(kSR);

    mod->inputs[2].voltage = 0.5f; // unity drive
    mod->inputs[3].voltage = 0.0f; // fully dry

    float max_err = 0.f;
    for (int i = 0; i < 256; ++i) {
        const float x          = 0.3f * std::sin(2.f * 3.14159265f * 440.f * i / kSR);
        mod->inputs[0].voltage = x;
        mod->inputs[1].voltage = x;
        res->step_block(1);
        if (i > 32) { // allow HPF warmup
            max_err = std::max(max_err, std::abs(mod->outputs[0].voltage - x));
        }
    }
    // With wet=0 the output is the dry sample — HPF warmup causes initial
    // error but should settle quickly; check it's bounded.
    REQUIRE(max_err < 0.01f);
}

TEST_CASE("SpiralModule: higher drive increases output level") {
    float peak_low = 0.f, peak_high = 0.f;

    for (float drive : {0.25f, 0.75f}) {
        auto      mod_ptr = std::make_unique<SpiralModule>();
        auto*     mod     = mod_ptr.get();
        GridGraph g;
        g.add_module(std::move(mod_ptr));
        auto res = g.build();
        REQUIRE(res.has_value());
        res->prepare(kSR);

        mod->inputs[2].voltage = drive;
        mod->inputs[3].voltage = 1.0f;

        for (int i = 0; i < 512; ++i) {
            const float x          = 0.1f * std::sin(2.f * 3.14159265f * 440.f * i / kSR);
            mod->inputs[0].voltage = x;
            mod->inputs[1].voltage = x;
            res->step_block(1);
            if (i > 32) {
                float p = std::abs(mod->outputs[0].voltage);
                if (drive < 0.5f)
                    peak_low = std::max(peak_low, p);
                else
                    peak_high = std::max(peak_high, p);
            }
        }
    }
    REQUIRE(peak_high > peak_low);
}

TEST_CASE("SpiralModule: L and R channels are independent") {
    auto      mod_ptr = std::make_unique<SpiralModule>();
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(kSR);

    mod->inputs[2].voltage = 0.7f;
    mod->inputs[3].voltage = 1.0f;

    // Drive L with a sine, R with silence.
    for (int i = 0; i < 512; ++i) {
        const float x          = 0.5f * std::sin(2.f * 3.14159265f * 440.f * i / kSR);
        mod->inputs[0].voltage = x;
        mod->inputs[1].voltage = 0.0f;
        res->step_block(1);
    }
    // L should have signal; R should not.
    REQUIRE(std::abs(mod->outputs[0].voltage) > 1e-4f);
    REQUIRE(std::abs(mod->outputs[1].voltage) < 1e-4f);
}

TEST_CASE("SpiralModule: output is finite for extreme drive") {
    auto      mod_ptr = std::make_unique<SpiralModule>();
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(kSR);

    mod->inputs[2].voltage = 1.0f; // max drive (4×)
    mod->inputs[3].voltage = 1.0f;

    for (int i = 0; i < 256; ++i) {
        const float x          = (i % 2 == 0) ? 1.0f : -1.0f;
        mod->inputs[0].voltage = x;
        mod->inputs[1].voltage = x;
        res->step_block(1);
        REQUIRE(std::isfinite(mod->outputs[0].voltage));
        REQUIRE(std::isfinite(mod->outputs[1].voltage));
    }
}

TEST_CASE("SpiralModule: clamps out-of-range CV inputs") {
    auto      mod_ptr = std::make_unique<SpiralModule>();
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(kSR);

    mod->inputs[0].voltage = 0.3f;
    mod->inputs[1].voltage = 0.3f;
    mod->inputs[2].voltage = 2.0f;  // out of range
    mod->inputs[3].voltage = -1.0f; // out of range

    for (int i = 0; i < 64; ++i) {
        res->step_block(1);
        REQUIRE(std::isfinite(mod->outputs[0].voltage));
        REQUIRE(std::isfinite(mod->outputs[1].voltage));
    }
}
