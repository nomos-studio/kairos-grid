// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for ADSRModule and SimpleLFOModule.

#include <kairos_grid/grid_graph.hpp>
#include <kairos_grid/surge/surge_modulator_modules.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>

using namespace kairos_grid;
using namespace kairos_grid::surge;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Prepare a standalone module (no GridGraph) at 48 kHz.
static void prepare_at_48k(GridModule& m) {
    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    m.prepare(args);
}

// Step a standalone module n_samples times.
static void step_module(GridModule& m, int n_samples) {
    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    for (int i = 0; i < n_samples; ++i) {
        args.frame = static_cast<int64_t>(i);
        m.process(args);
    }
}

// Fast A/D/R = 1.0 (shortest ~100 ms at 48 kHz/32), sustain = 0.7.
static void set_adsr_params(ADSRModule& m, float s = 0.7f) {
    m.inputs[ADSRModule::k_attack].voltage  = 1.0f;
    m.inputs[ADSRModule::k_decay].voltage   = 1.0f;
    m.inputs[ADSRModule::k_sustain].voltage = s;
    m.inputs[ADSRModule::k_release].voltage = 1.0f;
}

// ---------------------------------------------------------------------------
// ADSRModule — structure
// ---------------------------------------------------------------------------

TEST_CASE("ADSRModule: correct port counts") {
    ADSRModule m;
    REQUIRE(m.inputs.size() == 5);
    REQUIRE(m.outputs.size() == 1);
}

TEST_CASE("ADSRModule: declares one performance tap named signal/envelope") {
    ADSRModule m;
    REQUIRE(m.taps.size() == 1);
    REQUIRE(m.taps[0].name == "signal/envelope");
}

TEST_CASE("ADSRModule: declares four named param ports") {
    ADSRModule m;
    REQUIRE(m.param_ports.size() == 4);
    REQUIRE(m.param_ports[0].name == "adsr/attack");
    REQUIRE(m.param_ports[1].name == "adsr/decay");
    REQUIRE(m.param_ports[2].name == "adsr/sustain");
    REQUIRE(m.param_ports[3].name == "adsr/release");
}

TEST_CASE("ADSRModule: param port indices match Input enum") {
    ADSRModule m;
    REQUIRE(m.param_ports[0].port_idx == ADSRModule::k_attack);
    REQUIRE(m.param_ports[1].port_idx == ADSRModule::k_decay);
    REQUIRE(m.param_ports[2].port_idx == ADSRModule::k_sustain);
    REQUIRE(m.param_ports[3].port_idx == ADSRModule::k_release);
}

// ---------------------------------------------------------------------------
// ADSRModule — envelope behaviour
// ---------------------------------------------------------------------------

TEST_CASE("ADSRModule: output is zero with no gate") {
    ADSRModule m;
    prepare_at_48k(m);
    set_adsr_params(m);
    m.inputs[ADSRModule::k_gate].voltage = 0.f;
    step_module(m, 32);
    REQUIRE_THAT(m.outputs[ADSRModule::k_env_out].voltage, Catch::Matchers::WithinAbs(0.f, 1e-6f));
}

TEST_CASE("ADSRModule: output rises after gate goes high") {
    ADSRModule m;
    prepare_at_48k(m);
    set_adsr_params(m);
    m.inputs[ADSRModule::k_gate].voltage = 1.f;
    step_module(m, 200);
    REQUIRE(m.outputs[ADSRModule::k_env_out].voltage > 0.01f);
}

TEST_CASE("ADSRModule: tap matches output") {
    ADSRModule m;
    prepare_at_48k(m);
    set_adsr_params(m);
    m.inputs[ADSRModule::k_gate].voltage = 1.f;
    step_module(m, 500);
    REQUIRE_THAT(m.taps[0].value,
                 Catch::Matchers::WithinAbs(m.outputs[ADSRModule::k_env_out].voltage, 1e-9f));
}

TEST_CASE("ADSRModule: output is bounded [0, 1] during full ADSR cycle") {
    ADSRModule m;
    prepare_at_48k(m);
    set_adsr_params(m);

    bool            out_of_range = false;
    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};

    // Attack + decay phase with gate held.
    m.inputs[ADSRModule::k_gate].voltage = 1.f;
    for (int i = 0; i < 12000; ++i) {
        args.frame = i;
        m.process(args);
        const float v = m.outputs[ADSRModule::k_env_out].voltage;
        if (v < -1e-4f || v > 1.0001f)
            out_of_range = true;
    }

    // Release phase.
    m.inputs[ADSRModule::k_gate].voltage = 0.f;
    for (int i = 0; i < 6000; ++i) {
        args.frame = 12000 + i;
        m.process(args);
        const float v = m.outputs[ADSRModule::k_env_out].voltage;
        if (v < -1e-4f || v > 1.0001f)
            out_of_range = true;
    }

    REQUIRE_FALSE(out_of_range);
}

TEST_CASE("ADSRModule: output settles near sustain with gate held") {
    // a=d=r=1 → ~150 blocks each stage. After 375 blocks (12000 samples)
    // attack and decay are complete and we are in the sustain stage.
    ADSRModule m;
    prepare_at_48k(m);
    set_adsr_params(m, 0.7f);
    m.inputs[ADSRModule::k_gate].voltage = 1.f;
    step_module(m, 12000);
    REQUIRE_THAT(m.outputs[ADSRModule::k_env_out].voltage, Catch::Matchers::WithinAbs(0.7f, 0.05f));
}

TEST_CASE("ADSRModule: output decays to near zero after gate release") {
    ADSRModule m;
    prepare_at_48k(m);
    set_adsr_params(m, 0.7f);

    // Let it reach sustain first.
    m.inputs[ADSRModule::k_gate].voltage = 1.f;
    step_module(m, 12000);

    // Release.
    m.inputs[ADSRModule::k_gate].voltage = 0.f;
    step_module(m, 6000); // > 150 blocks release time

    REQUIRE(m.outputs[ADSRModule::k_env_out].voltage < 0.05f);
}

TEST_CASE("ADSRModule: retrigger from mid-envelope starts new attack") {
    ADSRModule m;
    prepare_at_48k(m);
    set_adsr_params(m, 0.7f);

    // Gate on; reach sustain.
    m.inputs[ADSRModule::k_gate].voltage = 1.f;
    step_module(m, 12000);

    // Gate off for one block then back on — retrigger.
    m.inputs[ADSRModule::k_gate].voltage = 0.f;
    step_module(m, 32);
    m.inputs[ADSRModule::k_gate].voltage = 1.f;
    step_module(m, 200);

    // Envelope should be in attack/sustain cycle again, not at zero.
    REQUIRE(m.outputs[ADSRModule::k_env_out].voltage > 0.1f);
}

TEST_CASE("ADSRModule: integrates into GridEngine (port_schema has 4 entries)") {
    GridGraph g;
    g.add_module(std::make_unique<ADSRModule>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    REQUIRE(res->port_schema().size() == 4); // attack, decay, sustain, release
    REQUIRE(res->tap_schema().size() == 1);  // signal/envelope
    REQUIRE(res->tap_schema().entries[0].name == "signal/envelope");
}

TEST_CASE("ADSRModule: tap_frame reflects envelope after apply_params + step_block") {
    GridGraph g;
    g.add_module(std::make_unique<ADSRModule>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // Set gate directly, use apply_params for A/D/S/R.
    auto* adsr = dynamic_cast<ADSRModule*>(res->module(0));
    REQUIRE(adsr != nullptr);
    adsr->inputs[ADSRModule::k_gate].voltage = 1.f;

    // Write a=d=r=1, s=0.7 via param bus.
    std::array<float, 4> frame{};
    for (const auto& e : res->port_schema().entries) {
        if (e.name == "adsr/attack")
            frame[static_cast<std::size_t>(e.id)] = 1.0f;
        if (e.name == "adsr/decay")
            frame[static_cast<std::size_t>(e.id)] = 1.0f;
        if (e.name == "adsr/sustain")
            frame[static_cast<std::size_t>(e.id)] = 0.7f;
        if (e.name == "adsr/release")
            frame[static_cast<std::size_t>(e.id)] = 1.0f;
    }
    res->apply_params(frame);
    res->step_block(12000);

    const float tap_v = res->tap_frame()[0];
    REQUIRE_THAT(tap_v, Catch::Matchers::WithinAbs(0.7f, 0.05f));
}

// ---------------------------------------------------------------------------
// SimpleLFOModule — structure
// ---------------------------------------------------------------------------

TEST_CASE("SimpleLFOModule: correct port counts") {
    SimpleLFOModule m;
    REQUIRE(m.inputs.size() == 3);
    REQUIRE(m.outputs.size() == 1);
}

TEST_CASE("SimpleLFOModule: declares one performance tap named signal/lfo") {
    SimpleLFOModule m;
    REQUIRE(m.taps.size() == 1);
    REQUIRE(m.taps[0].name == "signal/lfo");
}

TEST_CASE("SimpleLFOModule: declares three named param ports") {
    SimpleLFOModule m;
    REQUIRE(m.param_ports.size() == 3);
    REQUIRE(m.param_ports[0].name == "lfo/rate");
    REQUIRE(m.param_ports[1].name == "lfo/deform");
    REQUIRE(m.param_ports[2].name == "lfo/shape");
}

// ---------------------------------------------------------------------------
// SimpleLFOModule — LFO behaviour
// ---------------------------------------------------------------------------

TEST_CASE("SimpleLFOModule: output is finite after warm-up") {
    SimpleLFOModule m;
    prepare_at_48k(m);
    m.inputs[SimpleLFOModule::k_rate].voltage   = -3.32f; // ~10 Hz
    m.inputs[SimpleLFOModule::k_shape].voltage  = 0.f;    // SINE
    m.inputs[SimpleLFOModule::k_deform].voltage = 0.f;
    step_module(m, 4800);
    REQUIRE(std::isfinite(m.outputs[SimpleLFOModule::k_lfo_out].voltage));
}

TEST_CASE("SimpleLFOModule: output is bounded [-1, 1] for sine") {
    SimpleLFOModule m;
    prepare_at_48k(m);
    m.inputs[SimpleLFOModule::k_rate].voltage   = -3.32f;
    m.inputs[SimpleLFOModule::k_shape].voltage  = 0.f; // SINE
    m.inputs[SimpleLFOModule::k_deform].voltage = 0.f;

    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    bool            out_of_range = false;
    for (int i = 0; i < 9600; ++i) {
        args.frame = i;
        m.process(args);
        const float v = m.outputs[SimpleLFOModule::k_lfo_out].voltage;
        if (v < -1.001f || v > 1.001f)
            out_of_range = true;
    }
    REQUIRE_FALSE(out_of_range);
}

TEST_CASE("SimpleLFOModule: sine output is bipolar over one full cycle") {
    // At rate=-3.32, freq≈10 Hz, full cycle ≈ 4800 samples.
    SimpleLFOModule m;
    prepare_at_48k(m);
    m.inputs[SimpleLFOModule::k_rate].voltage   = -3.32f;
    m.inputs[SimpleLFOModule::k_shape].voltage  = 0.f; // SINE
    m.inputs[SimpleLFOModule::k_deform].voltage = 0.f;

    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    float           max_v = 0.f, min_v = 0.f;
    for (int i = 0; i < 4800; ++i) {
        args.frame = i;
        m.process(args);
        const float v = m.outputs[SimpleLFOModule::k_lfo_out].voltage;
        if (v > max_v)
            max_v = v;
        if (v < min_v)
            min_v = v;
    }
    REQUIRE(max_v > 0.5f);
    REQUIRE(min_v < -0.5f);
}

TEST_CASE("SimpleLFOModule: tap matches output") {
    SimpleLFOModule m;
    prepare_at_48k(m);
    m.inputs[SimpleLFOModule::k_rate].voltage  = -3.32f;
    m.inputs[SimpleLFOModule::k_shape].voltage = 0.f;
    step_module(m, 1000);
    REQUIRE_THAT(m.taps[0].value,
                 Catch::Matchers::WithinAbs(m.outputs[SimpleLFOModule::k_lfo_out].voltage, 1e-9f));
}

TEST_CASE("SimpleLFOModule: ramp shape produces monotone segments") {
    SimpleLFOModule m;
    prepare_at_48k(m);
    m.inputs[SimpleLFOModule::k_rate].voltage   = -3.32f;
    m.inputs[SimpleLFOModule::k_shape].voltage  = 1.f; // RAMP
    m.inputs[SimpleLFOModule::k_deform].voltage = 0.f;

    // Run for just under one cycle — output should be generally increasing.
    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    float           first{0.f}, last{0.f};
    for (int i = 0; i < 4600; ++i) {
        args.frame = i;
        m.process(args);
        if (i == 100)
            first = m.outputs[SimpleLFOModule::k_lfo_out].voltage;
        if (i == 4599)
            last = m.outputs[SimpleLFOModule::k_lfo_out].voltage;
    }
    REQUIRE(last > first);
}

TEST_CASE("SimpleLFOModule: all 8 shapes produce finite output") {
    for (int shape = 0; shape < 8; ++shape) {
        SimpleLFOModule m;
        prepare_at_48k(m);
        m.inputs[SimpleLFOModule::k_rate].voltage   = -3.32f;
        m.inputs[SimpleLFOModule::k_shape].voltage  = static_cast<float>(shape);
        m.inputs[SimpleLFOModule::k_deform].voltage = 0.f;
        step_module(m, 4800);
        INFO("shape = " << shape);
        REQUIRE(std::isfinite(m.outputs[SimpleLFOModule::k_lfo_out].voltage));
    }
}

TEST_CASE("SimpleLFOModule: integrates into GridEngine, tap in tap_schema") {
    GridGraph g;
    g.add_module(std::make_unique<SimpleLFOModule>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    REQUIRE(res->tap_schema().size() == 1);
    REQUIRE(res->port_schema().size() == 3);
    REQUIRE(res->tap_schema().entries[0].name == "signal/lfo");
}

TEST_CASE("SimpleLFOModule: tap_frame is bipolar after one cycle via engine") {
    GridGraph g;
    g.add_module(std::make_unique<SimpleLFOModule>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    auto* lfo = dynamic_cast<SimpleLFOModule*>(res->module(0));
    REQUIRE(lfo != nullptr);
    lfo->inputs[SimpleLFOModule::k_rate].voltage  = -3.32f;
    lfo->inputs[SimpleLFOModule::k_shape].voltage = 0.f; // SINE

    float max_v = 0.f, min_v = 0.f;
    for (int block = 0; block < 150; ++block) {
        res->step_block(32);
        const float v = res->tap_frame()[0];
        if (v > max_v)
            max_v = v;
        if (v < min_v)
            min_v = v;
    }
    REQUIRE(max_v > 0.5f);
    REQUIRE(min_v < -0.5f);
}
