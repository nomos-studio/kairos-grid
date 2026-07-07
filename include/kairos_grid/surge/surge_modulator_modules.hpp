// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <kairos_grid/grid_module.hpp>
#include <sst/basic-blocks/modulators/ADSREnvelope.h>
#include <sst/basic-blocks/modulators/SimpleLFO.h>

#include <algorithm>
#include <cmath>

namespace kairos_grid::surge {

namespace detail {

    // Minimal SRProvider for sst-basic-blocks modulators.
    //
    // envelope_rate_linear_nowrap(x) returns BLOCK_SIZE * 2^x / samplerate,
    // matching the SurgeStorage table formula analytically.
    struct EnvSRProvider {
        float samplerate{48000.f};

        float envelope_rate_linear_nowrap(float x) const noexcept {
            return BLOCK_SIZE * std::exp2f(x) / samplerate;
        }
    };

} // namespace detail

// ---------------------------------------------------------------------------
// ADSRModule — digital ADSR envelope generator.
//
// Parameter semantics (rate, not time — consistent with Surge's internal scale):
//   0.0 = slowest (~256 s at 48 kHz/32),  1.0 = fastest (~100 ms).
//
// Inputs:
//   0  gate      > 0.5 = gate on; rising edge triggers attack
//   1  attack    [0, 1] rate
//   2  decay     [0, 1] rate
//   3  sustain   [0, 1] level
//   4  release   [0, 1] rate
//
// Outputs:
//   0  envelope  [0, 1]
//
// Named param ports: adsr/attack, adsr/decay, adsr/sustain, adsr/release
// Performance tap:   signal/envelope
// ---------------------------------------------------------------------------
class ADSRModule : public GridModule {
  public:
    enum Input {
        k_gate       = 0,
        k_attack     = 1,
        k_decay      = 2,
        k_sustain    = 3,
        k_release    = 4,
        k_num_inputs = 5,
    };
    enum Output {
        k_env_out     = 0,
        k_num_outputs = 1,
    };

    ADSRModule() : GridModule(k_num_inputs, k_num_outputs), env_(&sr_) {
        taps.push_back({"signal/envelope", 0.f});
        param_ports.push_back({"adsr/attack", k_attack});
        param_ports.push_back({"adsr/decay", k_decay});
        param_ports.push_back({"adsr/sustain", k_sustain});
        param_ports.push_back({"adsr/release", k_release});
    }

    void prepare(const GridProcessArgs& args) override {
        sr_.samplerate = args.sample_rate;
        env_.onSampleRateChanged();
    }

    void process(const GridProcessArgs&) override {
        const bool gate_now = inputs[k_gate].voltage > 0.5f;

        if (gate_now && !prev_gate_)
            env_.attackFrom(env_.output, 0.f, 1 /* linear */, true /* digital */);
        prev_gate_ = gate_now;

        const float a = std::clamp(inputs[k_attack].voltage, 0.f, 1.f);
        const float d = std::clamp(inputs[k_decay].voltage, 0.f, 1.f);
        const float s = std::clamp(inputs[k_sustain].voltage, 0.f, 1.f);
        const float r = std::clamp(inputs[k_release].voltage, 0.f, 1.f);

        env_.process(a, d, s, r, 1, 1, 1, gate_now);

        const float v              = env_.output;
        outputs[k_env_out].voltage = v;
        taps[0].value              = v;
    }

  private:
    detail::EnvSRProvider sr_;
    // ADSREnvelope stores a raw pointer to sr_; ADSRModule must not be moved.
    sst::basic_blocks::modulators::ADSREnvelope<detail::EnvSRProvider, BLOCK_SIZE> env_;
    bool prev_gate_{false};
};

// ---------------------------------------------------------------------------
// SimpleLFOModule — 8-shape LFO using sst-basic-blocks SimpleLFO.
//
// LFO frequency: freq_Hz ≈ 2^(-rate).
//   rate =  0.00  →  1 Hz
//   rate = -3.32  → 10 Hz   (fast modulation)
//   rate = +3.32  →  0.1 Hz (slow modulation)
//
// Inputs:
//   0  rate    rate parameter; typical range [-5, 5]
//   1  deform  [-1, 1] waveform deformation
//   2  shape   0=SINE  1=RAMP  2=DOWN_RAMP  3=TRI
//              4=PULSE 5=SMOOTH_NOISE 6=SH_NOISE 7=RANDOM_TRIGGER
//
// Outputs:
//   0  lfo_out  [-1, 1]
//
// Named param ports: lfo/rate, lfo/deform, lfo/shape
// Performance tap:   signal/lfo
// ---------------------------------------------------------------------------
class SimpleLFOModule : public GridModule {
  public:
    enum Input {
        k_rate       = 0,
        k_deform     = 1,
        k_shape      = 2,
        k_num_inputs = 3,
    };
    enum Output {
        k_lfo_out     = 0,
        k_num_outputs = 1,
    };

    SimpleLFOModule() : GridModule(k_num_inputs, k_num_outputs), lfo_(&sr_) {
        taps.push_back({"signal/lfo", 0.f});
        param_ports.push_back({"lfo/rate", k_rate});
        param_ports.push_back({"lfo/deform", k_deform});
        param_ports.push_back({"lfo/shape", k_shape});
        block_pos_ = BLOCK_SIZE;
    }

    void prepare(const GridProcessArgs& args) override {
        sr_.samplerate = args.sample_rate;
        lfo_.attack(0);
        block_pos_ = BLOCK_SIZE;
    }

    void process(const GridProcessArgs&) override {
        if (block_pos_ >= BLOCK_SIZE) {
            const float rate   = inputs[k_rate].voltage;
            const float deform = std::clamp(inputs[k_deform].voltage, -1.f, 1.f);
            const int   shape  = std::clamp(static_cast<int>(inputs[k_shape].voltage), 0, 7);
            lfo_.process_block(rate, deform, shape);
            block_pos_ = 0;
        }

        const float v              = lfo_.outputBlock[static_cast<std::size_t>(block_pos_)];
        outputs[k_lfo_out].voltage = v;
        taps[0].value              = v;
        ++block_pos_;
    }

  private:
    detail::EnvSRProvider sr_;
    // SimpleLFO is non-movable; SimpleLFOModule must not be moved.
    sst::basic_blocks::modulators::SimpleLFO<detail::EnvSRProvider, BLOCK_SIZE> lfo_;
    int block_pos_{BLOCK_SIZE};
};

} // namespace kairos_grid::surge
