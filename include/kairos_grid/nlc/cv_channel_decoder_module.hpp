// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <kairos_grid/grid_module.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace kairos_grid::nlc {

// ---------------------------------------------------------------------------
// CvChannelDecoderModule — continuous CV to structured gate array.
//
// Divides the [0,1] input range into N equal bands. Each band fires a gate
// output when the SPAN input is within its window. SPACE controls window
// half-width: narrow SPACE = selective activation; wide SPACE = overlapping
// bands, possible simultaneous multi-channel activity.
//
// With clock input: gate outputs update only on rising clock edges (sample-
// and-hold per clock cycle). Without clock (or before any clock rise): gates
// update every sample (free-run).
//
// Degenerate case N=1: Schmitt-trigger comparator. Single gate fires when
// input is within [0.5 − hw, 0.5 + hw] where hw = space × 0.5.
//
// Velocity output: tracks |dSPAN/dt| (rate of change in units of full range
// per second, clamped to [0,1]). Dense during rapid CV motion; zero when
// signal is static.
//
// Inputs:
//   0  span      [0,1] CV to decode
//   1  clock     Optional gate — rising edge triggers gate update
//   2  channels  Band count [1,8] as float (rounded, param-bus addressable)
//   3  space     Band half-width factor [0,2] (param-bus addressable)
//
// Outputs:
//   0–7  ch0..ch7  Gate per band (0.f or 1.f); unused bands stay 0
//   8    velocity  |dSPAN/dt| clamped to [0,1]
//
// Named param ports: cvdec/channels, cvdec/space
// Performance tap:   signal/velocity
// ---------------------------------------------------------------------------
class CvChannelDecoderModule : public GridModule {
  public:
    static constexpr int k_max_channels = 8;

    enum Input {
        k_span       = 0,
        k_clock      = 1,
        k_channels   = 2,
        k_space      = 3,
        k_num_inputs = 4,
    };
    enum Output {
        k_ch0         = 0,
        k_ch1         = 1,
        k_ch2         = 2,
        k_ch3         = 3,
        k_ch4         = 4,
        k_ch5         = 5,
        k_ch6         = 6,
        k_ch7         = 7,
        k_velocity    = 8,
        k_num_outputs = 9,
    };

    CvChannelDecoderModule() : GridModule(k_num_inputs, k_num_outputs) {
        taps.push_back({"signal/velocity", 0.f});
        param_ports.push_back({"cvdec/channels", k_channels});
        param_ports.push_back({"cvdec/space",    k_space});

        // Sensible defaults before the param bus writes anything.
        inputs[k_channels].voltage = 1.f;   // Schmitt trigger
        inputs[k_space].voltage    = 0.3f;
    }

    void prepare(const GridProcessArgs&) override {
        last_span_ = 0.f;
        prev_clk_  = false;
        clocked_   = false;
        for (auto& p : outputs) p.voltage = 0.f;
    }

    void process(const GridProcessArgs& args) override {
        const float span     = inputs[k_span].voltage;
        const bool  clk_now  = inputs[k_clock].voltage > 0.5f;
        const bool  clk_rise = clk_now && !prev_clk_;

        if (clk_rise) clocked_ = true;
        const bool update = !clocked_ || clk_rise;

        if (update) {
            const int n = std::clamp(
                static_cast<int>(inputs[k_channels].voltage + 0.5f), 1, k_max_channels);
            const float sp = std::clamp(inputs[k_space].voltage, 0.f, 2.f);
            const float hw = sp * 0.5f / static_cast<float>(n);

            for (int i = 0; i < k_max_channels; ++i) {
                if (i < n) {
                    const float center = (n == 1)
                        ? 0.5f
                        : (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
                    outputs[k_ch0 + i].voltage =
                        (std::abs(span - center) <= hw) ? 1.f : 0.f;
                } else {
                    outputs[k_ch0 + i].voltage = 0.f;
                }
            }
        }

        const float vel =
            std::abs(span - last_span_) * args.sample_rate;
        outputs[k_velocity].voltage = std::clamp(vel, 0.f, 1.f);
        taps[0].value = outputs[k_velocity].voltage;

        last_span_ = span;
        prev_clk_  = clk_now;
    }

  private:
    float last_span_{0.f};
    bool  prev_clk_ {false};
    bool  clocked_  {false};
};

} // namespace kairos_grid::nlc
