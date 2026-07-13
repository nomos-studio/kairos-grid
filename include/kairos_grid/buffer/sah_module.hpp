// SPDX-License-Identifier: GPL-3.0-or-later
// sah — sample-and-hold, dual-use: modulation S&H and sample-rate reduction.
//
// Both uses are the same circuit: latch the input to the output on a trigger event,
// hold until the next trigger.  What varies is the trigger source:
//   Modulation S&H  — external trigger, rising-edge detected on inputs[2]
//   SR reduction    — internal periodic counter controlled by inputs[3] (rate CV)
// Both sources are active simultaneously; whichever fires first latches this block.
//
// SR reduction (rate > 0):
//   hold_period = 1 + round(rate × 4799)
//   rate=0       → period=1 → passthrough (every block is a latch)
//   rate≈0.0002  → period=2 (≈24 kHz effective SR)
//   rate=0.5     → period≈2401 (≈20 Hz effective SR)
//   rate=1       → period=4800 (≈10 Hz effective SR)
//
// Modulation S&H (trig input):
//   Rising edge detection: latch when trig crosses 0.5V from below.
//   No latch on sustained high or falling edge.
//
// Ports (4 inputs, 2 outputs):
//   inputs[0]  in-l  — signal to sample (left)
//   inputs[1]  in-r  — signal to sample (right)
//   inputs[2]  trig  — external trigger; latch on rising edge (threshold 0.5V)
//   inputs[3]  rate  — decimation rate CV [0, 1]; 0 = external-trigger-only mode
//   outputs[0] out-l — held value (left)
//   outputs[1] out-r — held value (right)
//
// Stereo: L and R share the same trigger logic but hold independent values.
//
// prepare() resets held values, trigger state, and counter.

#pragma once

#include <kairos_grid/grid_module.hpp>

#include <algorithm>
#include <cmath>

namespace kairos_grid {

class SahModule : public GridModule {
  public:
    SahModule() : GridModule(4, 2) {}

    void prepare(const GridProcessArgs&) override {
        held_l_    = 0.f;
        held_r_    = 0.f;
        prev_trig_ = 0.f;
        counter_   = 0;
    }

    void process(const GridProcessArgs&) override {
        bool latch = false;

        const float rate = std::clamp(inputs[3].voltage, 0.f, 1.f);
        if (rate > 0.f) {
            const int period = 1 + static_cast<int>(rate * 4799.f);
            if (++counter_ >= period) {
                counter_ = 0;
                latch    = true;
            }
        }

        const float trig = inputs[2].voltage;
        if (trig > 0.5f && prev_trig_ <= 0.5f)
            latch = true;
        prev_trig_ = trig;

        if (latch) {
            held_l_ = inputs[0].voltage;
            held_r_ = inputs[1].voltage;
        }

        outputs[0].voltage = held_l_;
        outputs[1].voltage = held_r_;
    }

  private:
    float held_l_    = 0.f;
    float held_r_    = 0.f;
    float prev_trig_ = 0.f;
    int   counter_   = 0;
};

} // namespace kairos_grid
