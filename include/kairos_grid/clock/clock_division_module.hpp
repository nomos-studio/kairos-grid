// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <kairos_grid/grid_module.hpp>

#include <algorithm>
#include <cmath>

namespace kairos_grid {

// ---------------------------------------------------------------------------
// ClockDivisionModule — beat-phase subdivision gate generator.
//
// Derives a subdivided clock gate from an incoming beat_phase signal
// ([0, 1) ramp per beat, typically from EnvironmentModule::k_beat_phase).
// The output is a binary gate that fires 1/division times per beat, with
// configurable pulse width and phase offset within each subdivision period.
//
// This is the kairos-grid equivalent of a Pam's New Workout clock output:
// a beat-clock subdivision derived sample-accurately from the Link beat
// phase rather than from a hardware clock edge. In a grid patch, connect
// the EnvironmentModule's beat_phase output here; the gate output drives
// whatever the subdivision needs to trigger.
//
// Inputs:
//   0  beat_phase    [0, 1)   incoming beat-phase ramp (one cycle per beat,
//                             from EnvironmentModule::k_beat_phase)
//   1  division      > 0      subdivision period in beats:
//                               1.0     = 1 pulse/beat  (quarter notes at 4/4)
//                               0.5     = 2 pulses/beat (8th notes)
//                               0.25    = 4 pulses/beat (16th notes)
//                               0.125   = 8 pulses/beat (32nd notes)
//                               1.0/3.0 = 3 pulses/beat (triplet quarters)
//                               1.0/6.0 = 6 pulses/beat (triplet 8ths)
//   2  pulse_width   [0, 1]   fraction of each subdivision period gate is high
//   3  phase_offset  [0, 1]   phase shift within each subdivision period
//
// Outputs:
//   0  gate   0.0 (off) or 1.0 (on)
//
// Named param ports: clock/division, clock/pulse_width, clock/phase_offset
// Performance tap:   signal/gate
//
// Gate logic (per sample):
//   sub_phase    = fmod(beat_phase / division,   1.0)
//   offset_phase = fmod(sub_phase  + phase_offset, 1.0)
//   gate         = (offset_phase < pulse_width) ? 1.0 : 0.0
//
// Since beat_phase is updated at block rate by the EnvironmentModule param bus,
// gate output is effectively constant within a block — block-rate accuracy,
// which is sufficient for all subdivision rates down to 1/32 at 200 BPM.
// ---------------------------------------------------------------------------
class ClockDivisionModule : public GridModule {
  public:
    enum Input {
        k_beat_phase   = 0,
        k_division     = 1,
        k_pulse_width  = 2,
        k_phase_offset = 3,
        k_num_inputs   = 4,
    };
    enum Output {
        k_gate_out    = 0,
        k_num_outputs = 1,
    };

    ClockDivisionModule() : GridModule(k_num_inputs, k_num_outputs) {
        taps.push_back({"signal/gate", 0.f});
        param_ports.push_back({"clock/division", k_division});
        param_ports.push_back({"clock/pulse_width", k_pulse_width});
        param_ports.push_back({"clock/phase_offset", k_phase_offset});
    }

    void process(const GridProcessArgs&) override {
        const float beat_phase   = inputs[k_beat_phase].voltage;
        const float division     = std::max(1.f / 128.f, inputs[k_division].voltage);
        const float pulse_width  = std::clamp(inputs[k_pulse_width].voltage, 0.f, 1.f);
        const float phase_offset = inputs[k_phase_offset].voltage;

        const float sub_phase    = std::fmod(beat_phase / division, 1.f);
        const float offset_phase = std::fmod(sub_phase + phase_offset, 1.f);
        const float gate         = (offset_phase < pulse_width) ? 1.f : 0.f;

        outputs[k_gate_out].voltage = gate;
        taps[0].value               = gate;
    }
};

} // namespace kairos_grid
