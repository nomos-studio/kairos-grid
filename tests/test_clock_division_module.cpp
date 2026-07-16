// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/clock/clock_division_module.hpp>
#include <kairos_grid/grid_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace kairos_grid;

static const GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};

static void set_inputs(ClockDivisionModule& m, float beat_phase, float division = 1.f,
                       float pulse_width = 0.5f, float phase_offset = 0.f) {
    m.inputs[ClockDivisionModule::k_beat_phase].voltage   = beat_phase;
    m.inputs[ClockDivisionModule::k_division].voltage     = division;
    m.inputs[ClockDivisionModule::k_pulse_width].voltage  = pulse_width;
    m.inputs[ClockDivisionModule::k_phase_offset].voltage = phase_offset;
}

// ---------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------

TEST_CASE("ClockDivisionModule: port counts — 4 inputs, 1 output", "[clock]") {
    ClockDivisionModule m;
    REQUIRE(m.inputs.size() == 4);
    REQUIRE(m.outputs.size() == 1);
}

TEST_CASE("ClockDivisionModule: one performance tap named signal/gate", "[clock]") {
    ClockDivisionModule m;
    REQUIRE(m.taps.size() == 1);
    REQUIRE(m.taps[0].name == "signal/gate");
}

TEST_CASE("ClockDivisionModule: three named param ports", "[clock]") {
    ClockDivisionModule m;
    REQUIRE(m.param_ports.size() == 3);
    REQUIRE(m.param_ports[0].name == "clock/division");
    REQUIRE(m.param_ports[1].name == "clock/pulse_width");
    REQUIRE(m.param_ports[2].name == "clock/phase_offset");
}

TEST_CASE("ClockDivisionModule: param port indices match Input enum", "[clock]") {
    ClockDivisionModule m;
    REQUIRE(m.param_ports[0].port_idx == ClockDivisionModule::k_division);
    REQUIRE(m.param_ports[1].port_idx == ClockDivisionModule::k_pulse_width);
    REQUIRE(m.param_ports[2].port_idx == ClockDivisionModule::k_phase_offset);
}

TEST_CASE("ClockDivisionModule: non-copyable", "[clock]") {
    static_assert(!std::is_copy_constructible_v<ClockDivisionModule>);
    static_assert(!std::is_copy_assignable_v<ClockDivisionModule>);
}

TEST_CASE("ClockDivisionModule: gate is 0 before any process call", "[clock]") {
    ClockDivisionModule m;
    REQUIRE(m.outputs[ClockDivisionModule::k_gate_out].voltage == 0.f);
}

// ---------------------------------------------------------------------------
// Basic gate logic
// ---------------------------------------------------------------------------

TEST_CASE("ClockDivisionModule: gate is 0 when pulse_width is 0", "[clock]") {
    ClockDivisionModule m;
    // With pulse_width=0, offset_phase < 0 is never true — gate always off.
    for (float bp : {0.f, 0.1f, 0.5f, 0.9f}) {
        set_inputs(m, bp, 1.f, 0.f, 0.f);
        m.process(kArgs);
        INFO("beat_phase = " << bp);
        REQUIRE(m.outputs[ClockDivisionModule::k_gate_out].voltage == 0.f);
    }
}

TEST_CASE("ClockDivisionModule: gate is 1 when pulse_width is 1", "[clock]") {
    ClockDivisionModule m;
    // With pulse_width=1, offset_phase < 1 is always true.
    for (float bp : {0.f, 0.25f, 0.5f, 0.75f, 0.99f}) {
        set_inputs(m, bp, 1.f, 1.f, 0.f);
        m.process(kArgs);
        INFO("beat_phase = " << bp);
        REQUIRE(m.outputs[ClockDivisionModule::k_gate_out].voltage == 1.f);
    }
}

TEST_CASE("ClockDivisionModule: gate is 1 at beat_phase 0 with default pulse_width 0.5",
          "[clock]") {
    ClockDivisionModule m;
    set_inputs(m, 0.f, 1.f, 0.5f, 0.f);
    m.process(kArgs);
    REQUIRE(m.outputs[ClockDivisionModule::k_gate_out].voltage == 1.f);
}

TEST_CASE("ClockDivisionModule: gate is 0 past pulse_width in same period", "[clock]") {
    ClockDivisionModule m;
    // division=1, pulse_width=0.5: gate high for beat_phase in [0, 0.5), low in [0.5, 1)
    set_inputs(m, 0.6f, 1.f, 0.5f, 0.f);
    m.process(kArgs);
    REQUIRE(m.outputs[ClockDivisionModule::k_gate_out].voltage == 0.f);
}

TEST_CASE("ClockDivisionModule: tap matches output", "[clock]") {
    ClockDivisionModule m;
    set_inputs(m, 0.1f, 1.f, 0.5f, 0.f);
    m.process(kArgs);
    REQUIRE_THAT(m.taps[0].value, Catch::Matchers::WithinAbs(
                                      m.outputs[ClockDivisionModule::k_gate_out].voltage, 1e-9f));
}

// ---------------------------------------------------------------------------
// Subdivision
// ---------------------------------------------------------------------------

TEST_CASE("ClockDivisionModule: division 0.5 fires second pulse at mid-beat", "[clock]") {
    ClockDivisionModule m;
    // division=0.5: pulse fires at beat_phase 0 and 0.5
    set_inputs(m, 0.5f, 0.5f, 0.5f, 0.f);
    m.process(kArgs);
    REQUIRE(m.outputs[ClockDivisionModule::k_gate_out].voltage == 1.f);
}

TEST_CASE("ClockDivisionModule: division 0.5 is off between pulses", "[clock]") {
    ClockDivisionModule m;
    // At beat_phase=0.3, sub_phase=0.6 → gate off (pulse_width=0.5)
    set_inputs(m, 0.3f, 0.5f, 0.5f, 0.f);
    m.process(kArgs);
    REQUIRE(m.outputs[ClockDivisionModule::k_gate_out].voltage == 0.f);
}

TEST_CASE("ClockDivisionModule: four rising edges per beat at division 0.25", "[clock]") {
    // 16th-note subdivision: 4 gate pulses per beat
    ClockDivisionModule m;
    set_inputs(m, 0.f, 0.25f, 0.5f, 0.f);

    int   rising_edges = 0;
    float prev_gate    = 0.f; // gate is 0 at beat_phase just below 1.0

    constexpr int N = 10000;
    for (int i = 0; i < N; ++i) {
        m.inputs[ClockDivisionModule::k_beat_phase].voltage = float(i) / float(N);
        m.process(kArgs);
        const float gate = m.outputs[ClockDivisionModule::k_gate_out].voltage;
        if (gate > 0.5f && prev_gate < 0.5f)
            ++rising_edges;
        prev_gate = gate;
    }

    REQUIRE(rising_edges == 4);
}

TEST_CASE("ClockDivisionModule: triplet division fires three times per beat", "[clock]") {
    ClockDivisionModule m;
    // division=1/3: gate on at beat_phase 0, 1/3, 2/3
    const float div = 1.f / 3.f;

    set_inputs(m, 0.f, div, 0.4f, 0.f);
    m.process(kArgs);
    REQUIRE(m.outputs[ClockDivisionModule::k_gate_out].voltage == 1.f);

    set_inputs(m, 1.f / 3.f, div, 0.4f, 0.f);
    m.process(kArgs);
    REQUIRE(m.outputs[ClockDivisionModule::k_gate_out].voltage == 1.f);

    set_inputs(m, 2.f / 3.f, div, 0.4f, 0.f);
    m.process(kArgs);
    REQUIRE(m.outputs[ClockDivisionModule::k_gate_out].voltage == 1.f);
}

TEST_CASE("ClockDivisionModule: triplet division is off between pulses", "[clock]") {
    ClockDivisionModule m;
    const float         div = 1.f / 3.f;
    // At beat_phase=0.2, sub_phase=0.6 (0.2/(1/3)=0.6) → gate off (pulse_width=0.4)
    set_inputs(m, 0.2f, div, 0.4f, 0.f);
    m.process(kArgs);
    REQUIRE(m.outputs[ClockDivisionModule::k_gate_out].voltage == 0.f);
}

// ---------------------------------------------------------------------------
// Phase offset
// ---------------------------------------------------------------------------

TEST_CASE("ClockDivisionModule: phase_offset 0.5 shifts gate to second half", "[clock]") {
    ClockDivisionModule m;
    // division=1, pulse_width=0.5, phase_offset=0.5:
    //   sub_phase at beat_phase=0.0 → 0.0; offset_phase = 0.0+0.5 = 0.5
    //   0.5 < 0.5 is false → gate off
    set_inputs(m, 0.f, 1.f, 0.5f, 0.5f);
    m.process(kArgs);
    REQUIRE(m.outputs[ClockDivisionModule::k_gate_out].voltage == 0.f);
}

TEST_CASE("ClockDivisionModule: phase_offset 0.5 gate fires in second half", "[clock]") {
    ClockDivisionModule m;
    // With phase_offset=0.5, pulse_width=0.5:
    //   beat_phase=0.6 → sub_phase=0.6, offset_phase=fmod(0.6+0.5,1)=0.1 → gate on
    set_inputs(m, 0.6f, 1.f, 0.5f, 0.5f);
    m.process(kArgs);
    REQUIRE(m.outputs[ClockDivisionModule::k_gate_out].voltage == 1.f);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST_CASE("ClockDivisionModule: zero division input is clamped — no divide by zero", "[clock]") {
    ClockDivisionModule m;
    set_inputs(m, 0.f, 0.f, 0.5f, 0.f); // division=0 → clamped to 1/128
    m.process(kArgs);
    REQUIRE(std::isfinite(m.outputs[ClockDivisionModule::k_gate_out].voltage));
}

TEST_CASE("ClockDivisionModule: negative division input is clamped", "[clock]") {
    ClockDivisionModule m;
    set_inputs(m, 0.f, -1.f, 0.5f, 0.f);
    m.process(kArgs);
    REQUIRE(std::isfinite(m.outputs[ClockDivisionModule::k_gate_out].voltage));
}

// ---------------------------------------------------------------------------
// GridEngine integration
// ---------------------------------------------------------------------------

TEST_CASE("ClockDivisionModule: integrates into GridEngine", "[clock]") {
    GridGraph g;
    g.add_module(std::make_unique<ClockDivisionModule>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    REQUIRE(res->port_schema().size() == 3); // division, pulse_width, phase_offset
    REQUIRE(res->tap_schema().size() == 1);
    REQUIRE(res->tap_schema().entries[0].name == "signal/gate");
}

TEST_CASE("ClockDivisionModule: tap_frame reflects gate via engine step_block", "[clock]") {
    GridGraph g;
    g.add_module(std::make_unique<ClockDivisionModule>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    auto* m = dynamic_cast<ClockDivisionModule*>(res->module(0));
    REQUIRE(m != nullptr);

    // Set up: gate should be on (beat_phase=0, pulse_width=0.5 via param bus)
    m->inputs[ClockDivisionModule::k_beat_phase].voltage = 0.f;

    std::vector<float> frame(static_cast<std::size_t>(res->port_schema().size()), 0.f);
    for (const auto& e : res->port_schema().entries) {
        if (e.name == "clock/division")
            frame[static_cast<std::size_t>(e.id)] = 1.f;
        if (e.name == "clock/pulse_width")
            frame[static_cast<std::size_t>(e.id)] = 0.5f;
        if (e.name == "clock/phase_offset")
            frame[static_cast<std::size_t>(e.id)] = 0.f;
    }
    res->apply_params(frame);
    res->step_block(32);

    REQUIRE_THAT(res->tap_frame()[0], Catch::Matchers::WithinAbs(1.f, 1e-6f));
}
