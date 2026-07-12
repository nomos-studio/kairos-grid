// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for SurgeWaveshaperModule — 4-sample SIMD bridge to sst-waveshapers.

#include <kairos_grid/grid_engine.hpp>
#include <kairos_grid/grid_graph.hpp>
#include <kairos_grid/surge/surge_waveshaper_module.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace kairos_grid;
using namespace kairos_grid::surge;
using WS = sst::waveshapers::WaveshaperType;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static GridEngine build_shaper(WS type, float sr = 48000.f) {
    GridGraph g;
    g.add_module(std::make_unique<SurgeWaveshaperModule>(type));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(sr);
    return std::move(*res);
}

// Run and return the most recent L output.
static float run_n(GridEngine& eng, SurgeWaveshaperModule* mod, int n) {
    for (int i = 0; i < n; ++i)
        eng.step_block(1);
    return mod->outputs[0].voltage;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("SurgeWaveshaperModule: constructs with 3 inputs and 2 outputs") {
    SurgeWaveshaperModule m(WS::wst_soft);
    REQUIRE(m.inputs.size() == 3);
    REQUIRE(m.outputs.size() == 2);
}

// ---------------------------------------------------------------------------
// Zero-input → zero output
//
// After settling (≥ 4 samples for the buffer + extra for any internal state),
// a zero input with no drive should produce a near-zero output.  Tested for
// a representative set of types — skipping the rectifiers that deliberately
// produce DC from zero signal.
// ---------------------------------------------------------------------------

TEST_CASE("SurgeWaveshaperModule: zero input → zero output for saturators") {
    static constexpr WS kTypes[] = {WS::wst_soft, WS::wst_hard, WS::wst_asym, WS::wst_zamsat,
                                    WS::wst_ojd};
    for (auto t : kTypes) {
        auto      mod_ptr = std::make_unique<SurgeWaveshaperModule>(t);
        auto*     mod     = mod_ptr.get();
        GridGraph g;
        g.add_module(std::move(mod_ptr));
        auto res = g.build();
        REQUIRE(res.has_value());
        res->prepare(48000.f);

        mod->inputs[0].voltage = 0.f;
        mod->inputs[1].voltage = 0.f;
        mod->inputs[2].voltage = 0.f; // drive = 1× (unity)
        res->step_block(200);

        REQUIRE_THAT(mod->outputs[0].voltage, Catch::Matchers::WithinAbs(0.f, 0.05f));
        REQUIRE_THAT(mod->outputs[1].voltage, Catch::Matchers::WithinAbs(0.f, 0.05f));
    }
}

TEST_CASE("SurgeWaveshaperModule: zero input → zero output for wavefolders") {
    static constexpr WS kTypes[] = {WS::wst_westfold, WS::wst_dualfold, WS::wst_softfold};
    for (auto t : kTypes) {
        auto      mod_ptr = std::make_unique<SurgeWaveshaperModule>(t);
        auto*     mod     = mod_ptr.get();
        GridGraph g;
        g.add_module(std::move(mod_ptr));
        auto res = g.build();
        REQUIRE(res.has_value());
        res->prepare(48000.f);

        mod->inputs[0].voltage = 0.f;
        mod->inputs[1].voltage = 0.f;
        mod->inputs[2].voltage = 0.f;
        res->step_block(200);

        REQUIRE_THAT(mod->outputs[0].voltage, Catch::Matchers::WithinAbs(0.f, 0.05f));
        REQUIRE_THAT(mod->outputs[1].voltage, Catch::Matchers::WithinAbs(0.f, 0.05f));
    }
}

// ---------------------------------------------------------------------------
// Finite output — all 12 registered types
// ---------------------------------------------------------------------------

TEST_CASE("SurgeWaveshaperModule: output is finite for all registered types") {
    static constexpr WS kTypes[] = {
        WS::wst_soft,     WS::wst_hard,     WS::wst_asym,      WS::wst_zamsat,
        WS::wst_ojd,      WS::wst_fuzz,     WS::wst_fuzzheavy, WS::wst_westfold,
        WS::wst_dualfold, WS::wst_softfold, WS::wst_cheby2,    WS::wst_cheby3,
    };
    for (auto t : kTypes) {
        auto      mod_ptr = std::make_unique<SurgeWaveshaperModule>(t);
        auto*     mod     = mod_ptr.get();
        GridGraph g;
        g.add_module(std::move(mod_ptr));
        auto res = g.build();
        REQUIRE(res.has_value());
        res->prepare(48000.f);

        mod->inputs[0].voltage = 2.f;
        mod->inputs[1].voltage = 2.f;
        mod->inputs[2].voltage = 0.5f; // moderate drive

        bool finite = true;
        for (int i = 0; i < 4800; ++i) {
            res->step_block(1);
            if (!std::isfinite(mod->outputs[0].voltage) ||
                !std::isfinite(mod->outputs[1].voltage)) {
                finite = false;
                break;
            }
        }
        REQUIRE(finite);
    }
}

// ---------------------------------------------------------------------------
// Bounded output at high drive
// ---------------------------------------------------------------------------

TEST_CASE("SurgeWaveshaperModule: soft saturator output bounded at high drive") {
    auto      mod_ptr = std::make_unique<SurgeWaveshaperModule>(WS::wst_soft);
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[2].voltage = 1.f; // 16× pre-gain → heavy saturation

    bool        bounded  = true;
    const float inputs[] = {0.1f, 0.5f, 1.0f, 2.0f, -0.5f, -1.5f};
    for (float v : inputs) {
        mod->inputs[0].voltage = v;
        mod->inputs[1].voltage = v;
        for (int i = 0; i < 8; ++i)
            res->step_block(1);
        // After settling: saturator output ≤ ±1 V, scaled to ≤ ±5 V
        if (std::abs(mod->outputs[0].voltage) > 5.5f || std::abs(mod->outputs[1].voltage) > 5.5f) {
            bounded = false;
            break;
        }
    }
    REQUIRE(bounded);
}

TEST_CASE("SurgeWaveshaperModule: fuzz output bounded and finite at high drive") {
    auto      mod_ptr = std::make_unique<SurgeWaveshaperModule>(WS::wst_fuzzheavy);
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 3.f;
    mod->inputs[1].voltage = 3.f;
    mod->inputs[2].voltage = 1.f; // max drive

    bool ok = true;
    for (int i = 0; i < 9600; ++i) {
        res->step_block(1);
        const float L = mod->outputs[0].voltage;
        const float R = mod->outputs[1].voltage;
        if (!std::isfinite(L) || !std::isfinite(R) || std::abs(L) > 6.f) {
            ok = false;
            break;
        }
    }
    REQUIRE(ok);
}

// ---------------------------------------------------------------------------
// Stereo independence — L and R states are separate
// ---------------------------------------------------------------------------

TEST_CASE("SurgeWaveshaperModule: stereo channels processed independently") {
    // Stateful shapers accumulate per-channel state.  Feed L and R different
    // DC values; after settling the outputs should differ proportionally.
    auto      mod_ptr = std::make_unique<SurgeWaveshaperModule>(WS::wst_soft);
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 2.f;  // L — higher
    mod->inputs[1].voltage = 0.5f; // R — lower
    mod->inputs[2].voltage = 0.f;  // unity drive

    res->step_block(200); // settle

    const float L = mod->outputs[0].voltage;
    const float R = mod->outputs[1].voltage;

    REQUIRE(std::isfinite(L));
    REQUIRE(std::isfinite(R));
    // L-input was 4× larger; with soft saturation the outputs differ
    REQUIRE(std::abs(L) > std::abs(R));
}

// ---------------------------------------------------------------------------
// prepare() resets state — output starts from zero after re-prepare
// ---------------------------------------------------------------------------

TEST_CASE("SurgeWaveshaperModule: prepare() resets internal buffer and state") {
    auto      mod_ptr = std::make_unique<SurgeWaveshaperModule>(WS::wst_soft);
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // Drive hard to accumulate state
    mod->inputs[0].voltage = 5.f;
    mod->inputs[2].voltage = 1.f;
    res->step_block(1000);

    // Re-prepare resets the 4-sample ring buffer to 0
    res->prepare(48000.f);

    // Immediately after prepare, with zero input, output must be zero
    mod->inputs[0].voltage = 0.f;
    mod->inputs[2].voltage = 0.f;
    res->step_block(1);

    REQUIRE_THAT(mod->outputs[0].voltage, Catch::Matchers::WithinAbs(0.f, 1e-5f));
}

// ---------------------------------------------------------------------------
// 4-sample latency — output from sample N arrives at sample N+4
// ---------------------------------------------------------------------------

TEST_CASE("SurgeWaveshaperModule: 4-sample latency — first 4 outputs are zero") {
    // The 4-sample ring buffer emits 0 for the first 4 calls before any
    // processed block is available.  (Input is non-zero from call 1.)
    auto      mod_ptr = std::make_unique<SurgeWaveshaperModule>(WS::wst_soft);
    auto*     mod     = mod_ptr.get();
    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 5.f; // non-zero input from the start
    mod->inputs[1].voltage = 5.f;
    mod->inputs[2].voltage = 0.f;

    float outputs[4] = {};
    for (int i = 0; i < 4; ++i) {
        res->step_block(1);
        outputs[i] = mod->outputs[0].voltage;
    }

    // All 4 outputs before the first SIMD block completes must be zero
    for (int i = 0; i < 4; ++i) {
        REQUIRE_THAT(outputs[i], Catch::Matchers::WithinAbs(0.f, 1e-5f));
    }

    // 5th output is from the first processed SIMD block — should be non-zero
    res->step_block(1);
    REQUIRE(std::abs(mod->outputs[0].voltage) > 0.01f);
}
