// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for Surge XT oscillator module wrappers.

#include <kairos_grid/surge/surge_osc_module.hpp>
#include <kairos_grid/grid_engine.hpp>
#include <kairos_grid/grid_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace kairos_grid;
using namespace kairos_grid::surge;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static GridEngine build_single_surge(std::unique_ptr<GridModule> m)
{
    GridGraph g;
    g.add_module(std::move(m));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);
    return std::move(*res);
}

// ---------------------------------------------------------------------------
// ClassicOscModule
// ---------------------------------------------------------------------------

TEST_CASE("ClassicOscModule: constructs with 8 inputs and 2 outputs")
{
    ClassicOscModule m;
    REQUIRE(m.inputs.size()  == 8);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("ClassicOscModule: produces non-zero output for A4 after warm-up")
{
    auto  mod_ptr = std::make_unique<ClassicOscModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 69.f;  // A4 = 440 Hz

    // Warm up past the initial block.
    res->step_block(4800);  // 100 ms

    float out = mod->outputs[0].voltage;
    float aux = mod->outputs[1].voltage;

    REQUIRE(std::isfinite(out));
    REQUIRE(std::isfinite(aux));

    bool any_signal = (std::abs(out) > 1e-4f) || (std::abs(aux) > 1e-4f);
    REQUIRE(any_signal);
}

TEST_CASE("ClassicOscModule: output is finite and bounded after warm-up")
{
    // Surge's classic oscillator can peak slightly above ±1.0 (BLIT synthesis
    // artifact) before downstream gain / DC-blocking.  We scale by 5 V and
    // allow ±6 V headroom (1.2× amplitude), matching surge-rack practice.
    auto  mod_ptr = std::make_unique<ClassicOscModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 60.f;   // C4

    bool finite  = true;
    bool bounded = true;
    for (int i = 0; i < 9600; ++i)
    {
        res->step_block(1);
        float out = mod->outputs[0].voltage;
        float aux = mod->outputs[1].voltage;
        if (!std::isfinite(out) || !std::isfinite(aux))   { finite  = false; break; }
        if (std::abs(out) > 6.f  || std::abs(aux) > 6.f) { bounded = false; break; }
    }
    REQUIRE(finite);
    REQUIRE(bounded);
}

// ---------------------------------------------------------------------------
// SineOscModule
// ---------------------------------------------------------------------------

TEST_CASE("SineOscModule: constructs with correct port count")
{
    SineOscModule m;
    REQUIRE(m.inputs.size()  == 8);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("SineOscModule: produces finite output at C4")
{
    auto  mod_ptr = std::make_unique<SineOscModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 60.f;
    res->step_block(4800);

    REQUIRE(std::isfinite(mod->outputs[0].voltage));
    REQUIRE(std::isfinite(mod->outputs[1].voltage));
}

// ---------------------------------------------------------------------------
// FM2OscModule
// ---------------------------------------------------------------------------

TEST_CASE("FM2OscModule: produces finite output at A4")
{
    auto  mod_ptr = std::make_unique<FM2OscModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 69.f;
    res->step_block(4800);

    REQUIRE(std::isfinite(mod->outputs[0].voltage));
    REQUIRE(std::isfinite(mod->outputs[1].voltage));
}

// ---------------------------------------------------------------------------
// ModernOscModule
// ---------------------------------------------------------------------------

TEST_CASE("ModernOscModule: produces non-zero output")
{
    auto  mod_ptr = std::make_unique<ModernOscModule>();
    auto* mod     = mod_ptr.get();

    GridGraph g;
    g.add_module(std::move(mod_ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    mod->inputs[0].voltage = 60.f;
    res->step_block(4800);

    REQUIRE(std::isfinite(mod->outputs[0].voltage));
    bool any_signal = (std::abs(mod->outputs[0].voltage) > 1e-4f) ||
                      (std::abs(mod->outputs[1].voltage) > 1e-4f);
    REQUIRE(any_signal);
}

// ---------------------------------------------------------------------------
// Integration: ClassicOscModule → SVF filter (using GridGraph chain)
// ---------------------------------------------------------------------------

TEST_CASE("ClassicOscModule integrates into GridGraph chain (no MI, no crash)")
{
    // Build a two-module chain: classic osc → gain-passthrough via direct cable.
    // We don't have a filter module here, so we just verify the chain builds
    // and runs without crashing with surge in the mix.
    GridGraph g;
    int i_osc  = g.add_module(std::make_unique<ClassicOscModule>());
    int i_osc2 = g.add_module(std::make_unique<SineOscModule>());

    // No cables — each runs independently.
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    auto* c = dynamic_cast<ClassicOscModule*>(res->module(i_osc));
    REQUIRE(c != nullptr);
    c->inputs[0].voltage = 69.f;

    auto* s = dynamic_cast<SineOscModule*>(res->module(i_osc2));
    REQUIRE(s != nullptr);
    s->inputs[0].voltage = 60.f;

    res->step_block(4800);

    REQUIRE(std::isfinite(c->outputs[0].voltage));
    REQUIRE(std::isfinite(s->outputs[0].voltage));
}
