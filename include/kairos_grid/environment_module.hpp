// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <kairos_grid/grid_module.hpp>

namespace kairos_grid {

// Always-present module that bridges the nomos-rt param bus into the grid.
//
// nomos-rt writes environment signals (transport, voice, Link) before each
// block via GridEngine::apply_params() using the named param-bus ports
// declared here. process() forwards those values to output ports that are
// patchable within the graph like any other GridModule output.
//
// This module has no meaningful audio I/O — it is the sole conduit for
// block-rate control signals arriving from outside the grid. kairos-grid
// itself has no Link dependency; the Link peer in nomos-rt writes
// tempo/phase values into this module's param-bus inputs.
//
// Voltage conventions (enforced by caller, not by this module):
//   tempo_hz       — beats per second   (e.g. 120 BPM ≡ 2.0)
//   beat_phase     — [0, 1) ramp, one cycle per beat
//   bar_phase      — [0, 1) ramp, one cycle per bar
//   is_playing     — 0.0 = stopped, 1.0 = playing
//   voice_note     — MIDI note number 0–127
//   voice_gate     — 0.0 = note off, 1.0 = note on
//   voice_velocity — MIDI velocity 0–127
//
// Typical per-block usage:
//   eng.apply_params(env_frame);  // nomos-rt writes env signals by name
//   eng.step_block(BLOCK_SIZE);   // process() forwards them to patchable outputs
class EnvironmentModule : public GridModule {
  public:
    enum Port {
        k_tempo_hz       = 0,
        k_beat_phase     = 1,
        k_bar_phase      = 2,
        k_is_playing     = 3,
        k_voice_note     = 4,
        k_voice_gate     = 5,
        k_voice_velocity = 6,
        k_num_ports      = 7,
    };

    EnvironmentModule() : GridModule(k_num_ports, k_num_ports) {
        param_ports.reserve(k_num_ports);
        param_ports.push_back({"env/tempo_hz",       k_tempo_hz});
        param_ports.push_back({"env/beat_phase",     k_beat_phase});
        param_ports.push_back({"env/bar_phase",      k_bar_phase});
        param_ports.push_back({"env/is_playing",     k_is_playing});
        param_ports.push_back({"env/voice_note",     k_voice_note});
        param_ports.push_back({"env/voice_gate",     k_voice_gate});
        param_ports.push_back({"env/voice_velocity", k_voice_velocity});
    }

    void process(const GridProcessArgs&) override {
        for (int i = 0; i < k_num_ports; ++i)
            outputs[static_cast<std::size_t>(i)].voltage =
                inputs[static_cast<std::size_t>(i)].voltage;
    }
};

} // namespace kairos_grid
