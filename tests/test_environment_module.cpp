// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for EnvironmentModule.

#include <kairos_grid/environment_module.hpp>
#include <kairos_grid/grid_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>

using namespace kairos_grid;

// ---------------------------------------------------------------------------
// Local stubs — anonymous namespace to avoid ODR conflicts with other TUs.
// ---------------------------------------------------------------------------

namespace {

// Captures its single input voltage; used to test EnvironmentModule cabling.
class VoltageSink : public GridModule {
  public:
    VoltageSink() : GridModule(1, 0) {}
    void  process(const GridProcessArgs&) override { last = inputs[0].voltage; }
    float last{0.f};
};

} // namespace

// ---------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------

TEST_CASE("EnvironmentModule: correct port counts") {
    EnvironmentModule m;
    REQUIRE(m.inputs.size() == 7);
    REQUIRE(m.outputs.size() == 7);
    REQUIRE(m.param_ports.size() == 7);
}

TEST_CASE("EnvironmentModule: Port enum values are contiguous from 0") {
    REQUIRE(EnvironmentModule::k_tempo_hz == 0);
    REQUIRE(EnvironmentModule::k_beat_phase == 1);
    REQUIRE(EnvironmentModule::k_bar_phase == 2);
    REQUIRE(EnvironmentModule::k_is_playing == 3);
    REQUIRE(EnvironmentModule::k_voice_note == 4);
    REQUIRE(EnvironmentModule::k_voice_gate == 5);
    REQUIRE(EnvironmentModule::k_voice_velocity == 6);
    REQUIRE(EnvironmentModule::k_num_ports == 7);
}

TEST_CASE("EnvironmentModule: param port names are correct") {
    EnvironmentModule m;
    REQUIRE(m.param_ports[0].name == "env/tempo_hz");
    REQUIRE(m.param_ports[1].name == "env/beat_phase");
    REQUIRE(m.param_ports[2].name == "env/bar_phase");
    REQUIRE(m.param_ports[3].name == "env/is_playing");
    REQUIRE(m.param_ports[4].name == "env/voice_note");
    REQUIRE(m.param_ports[5].name == "env/voice_gate");
    REQUIRE(m.param_ports[6].name == "env/voice_velocity");
}

TEST_CASE("EnvironmentModule: param port indices match Port enum") {
    EnvironmentModule m;
    REQUIRE(m.param_ports[0].port_idx == EnvironmentModule::k_tempo_hz);
    REQUIRE(m.param_ports[1].port_idx == EnvironmentModule::k_beat_phase);
    REQUIRE(m.param_ports[2].port_idx == EnvironmentModule::k_bar_phase);
    REQUIRE(m.param_ports[3].port_idx == EnvironmentModule::k_is_playing);
    REQUIRE(m.param_ports[4].port_idx == EnvironmentModule::k_voice_note);
    REQUIRE(m.param_ports[5].port_idx == EnvironmentModule::k_voice_gate);
    REQUIRE(m.param_ports[6].port_idx == EnvironmentModule::k_voice_velocity);
}

// ---------------------------------------------------------------------------
// process() — direct (no engine)
// ---------------------------------------------------------------------------

TEST_CASE("EnvironmentModule: process forwards all inputs to outputs") {
    EnvironmentModule m;
    GridProcessArgs   args{48000.f, 1.f / 48000.f, 0};

    m.inputs[EnvironmentModule::k_tempo_hz].voltage       = 2.0f; // 120 BPM
    m.inputs[EnvironmentModule::k_beat_phase].voltage     = 0.5f;
    m.inputs[EnvironmentModule::k_bar_phase].voltage      = 0.25f;
    m.inputs[EnvironmentModule::k_is_playing].voltage     = 1.0f;
    m.inputs[EnvironmentModule::k_voice_note].voltage     = 60.f; // middle C
    m.inputs[EnvironmentModule::k_voice_gate].voltage     = 1.0f;
    m.inputs[EnvironmentModule::k_voice_velocity].voltage = 100.f;

    m.process(args);

    REQUIRE_THAT(m.outputs[EnvironmentModule::k_tempo_hz].voltage,
                 Catch::Matchers::WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(m.outputs[EnvironmentModule::k_beat_phase].voltage,
                 Catch::Matchers::WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(m.outputs[EnvironmentModule::k_bar_phase].voltage,
                 Catch::Matchers::WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(m.outputs[EnvironmentModule::k_is_playing].voltage,
                 Catch::Matchers::WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(m.outputs[EnvironmentModule::k_voice_note].voltage,
                 Catch::Matchers::WithinAbs(60.f, 1e-6f));
    REQUIRE_THAT(m.outputs[EnvironmentModule::k_voice_gate].voltage,
                 Catch::Matchers::WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(m.outputs[EnvironmentModule::k_voice_velocity].voltage,
                 Catch::Matchers::WithinAbs(100.f, 1e-6f));
}

TEST_CASE("EnvironmentModule: outputs are zero when inputs are zero") {
    EnvironmentModule m;
    GridProcessArgs   args{48000.f, 1.f / 48000.f, 0};
    m.process(args);

    for (int i = 0; i < EnvironmentModule::k_num_ports; ++i)
        REQUIRE_THAT(m.outputs[static_cast<std::size_t>(i)].voltage,
                     Catch::Matchers::WithinAbs(0.f, 1e-9f));
}

// ---------------------------------------------------------------------------
// Engine integration — port_schema, apply_params, step_block round-trips
// ---------------------------------------------------------------------------

TEST_CASE("EnvironmentModule: engine port_schema has 7 entries") {
    GridGraph g;
    g.add_module(std::make_unique<EnvironmentModule>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    REQUIRE(res->port_schema().size() == 7);
    REQUIRE(res->tap_schema().size() == 0); // no performance taps
}

TEST_CASE("EnvironmentModule: apply_params + step_block drives all outputs") {
    GridGraph g;
    int       env_idx = g.add_module(std::make_unique<EnvironmentModule>());
    auto      res     = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // Frame indexed by port_schema entry id — EnvironmentModule is the only
    // module so its param_ports map directly to ids 0..6.
    const std::array<float, 7> frame = {
        2.0f,  // tempo_hz       (120 BPM)
        0.75f, // beat_phase
        0.25f, // bar_phase
        1.0f,  // is_playing
        60.f,  // voice_note     (middle C)
        1.0f,  // voice_gate
        100.f, // voice_velocity
    };

    res->apply_params(frame);
    res->step_block(1);

    auto* env = dynamic_cast<EnvironmentModule*>(res->module(env_idx));
    REQUIRE(env != nullptr);

    REQUIRE_THAT(env->outputs[EnvironmentModule::k_tempo_hz].voltage,
                 Catch::Matchers::WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(env->outputs[EnvironmentModule::k_beat_phase].voltage,
                 Catch::Matchers::WithinAbs(0.75f, 1e-6f));
    REQUIRE_THAT(env->outputs[EnvironmentModule::k_voice_note].voltage,
                 Catch::Matchers::WithinAbs(60.f, 1e-6f));
    REQUIRE_THAT(env->outputs[EnvironmentModule::k_voice_gate].voltage,
                 Catch::Matchers::WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("EnvironmentModule: apply_params by port_schema name lookup") {
    GridGraph g;
    int       env_idx = g.add_module(std::make_unique<EnvironmentModule>());
    auto      res     = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // Build a frame by matching port_schema names — this is how nomos-rt
    // will actually drive the param bus (name-indexed, not position-indexed).
    std::array<float, 7> frame{};
    for (const auto& e : res->port_schema().entries) {
        if (e.name == "env/tempo_hz")
            frame[static_cast<std::size_t>(e.id)] = 3.0f; // 180 BPM
        if (e.name == "env/beat_phase")
            frame[static_cast<std::size_t>(e.id)] = 0.1f;
        if (e.name == "env/is_playing")
            frame[static_cast<std::size_t>(e.id)] = 1.0f;
        if (e.name == "env/voice_velocity")
            frame[static_cast<std::size_t>(e.id)] = 64.f;
    }

    res->apply_params(frame);
    res->step_block(1);

    auto* env = dynamic_cast<EnvironmentModule*>(res->module(env_idx));
    REQUIRE(env != nullptr);

    REQUIRE_THAT(env->outputs[EnvironmentModule::k_tempo_hz].voltage,
                 Catch::Matchers::WithinAbs(3.0f, 1e-6f));
    REQUIRE_THAT(env->outputs[EnvironmentModule::k_beat_phase].voltage,
                 Catch::Matchers::WithinAbs(0.1f, 1e-6f));
    REQUIRE_THAT(env->outputs[EnvironmentModule::k_is_playing].voltage,
                 Catch::Matchers::WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(env->outputs[EnvironmentModule::k_voice_velocity].voltage,
                 Catch::Matchers::WithinAbs(64.f, 1e-6f));
}

// ---------------------------------------------------------------------------
// Downstream cabling
// ---------------------------------------------------------------------------

TEST_CASE("EnvironmentModule: output can be cabled to a downstream module") {
    GridGraph g;
    int       env_idx  = g.add_module(std::make_unique<EnvironmentModule>());
    auto*     sink_raw = new VoltageSink;
    int       sink_idx = g.add_module(std::unique_ptr<VoltageSink>(sink_raw));

    // Cable beat_phase output → VoltageSink input.
    g.add_cable({env_idx, EnvironmentModule::k_beat_phase, sink_idx, 0});

    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // Write beat_phase = 0.6 via param bus.
    std::array<float, 7> frame{};
    for (const auto& e : res->port_schema().entries)
        if (e.name == "env/beat_phase")
            frame[static_cast<std::size_t>(e.id)] = 0.6f;

    res->apply_params(frame);
    // 2 samples: env sets output → cable routes → sink reads.
    res->step_block(2);

    REQUIRE_THAT(sink_raw->last, Catch::Matchers::WithinAbs(0.6f, 1e-6f));
}

TEST_CASE("EnvironmentModule: beat_phase updates propagate each block") {
    GridGraph g;
    int       env_idx = g.add_module(std::make_unique<EnvironmentModule>());
    auto      res     = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // Block 1: beat_phase = 0.25
    std::array<float, 7> frame{};
    auto                 set_beat = [&](float v) {
        for (const auto& e : res->port_schema().entries)
            if (e.name == "env/beat_phase")
                frame[static_cast<std::size_t>(e.id)] = v;
    };

    set_beat(0.25f);
    res->apply_params(frame);
    res->step_block(1);
    auto* env = dynamic_cast<EnvironmentModule*>(res->module(env_idx));
    REQUIRE_THAT(env->outputs[EnvironmentModule::k_beat_phase].voltage,
                 Catch::Matchers::WithinAbs(0.25f, 1e-6f));

    // Block 2: beat_phase = 0.50
    set_beat(0.50f);
    res->apply_params(frame);
    res->step_block(1);
    REQUIRE_THAT(env->outputs[EnvironmentModule::k_beat_phase].voltage,
                 Catch::Matchers::WithinAbs(0.50f, 1e-6f));

    // Block 3: beat_phase = 0.75
    set_beat(0.75f);
    res->apply_params(frame);
    res->step_block(1);
    REQUIRE_THAT(env->outputs[EnvironmentModule::k_beat_phase].voltage,
                 Catch::Matchers::WithinAbs(0.75f, 1e-6f));
}
