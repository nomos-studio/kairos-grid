// SPDX-License-Identifier: GPL-3.0-or-later
// Surge XT sst-waveshapers as GridModule — 4-sample SIMD bridge.
//
// sst-waveshapers processes 4 samples at a time via __m128 SIMD.  To bridge
// the per-sample grid interface this module buffers 4 inputs, calls the SIMD
// shaper once per 4-sample block, then emits the 4 processed outputs over the
// following 4 calls.  The result is exactly 4 samples (~0.08 ms at 48 kHz)
// of latency — imperceptible at any musical tempo.
//
// Separate QuadWaveshaperState instances for L and R channels preserve stereo
// independence for stateful shapers (those that maintain DC blocker or
// feedback registers across calls).
//
// Voltage convention: grid ports use ±5 V nominal; sst-waveshapers expect
// ±1 V.  Inputs are scaled ×0.2 before processing; outputs are scaled ×5.
// The drive taper (exponential, 1×–16×) is applied inside the SIMD call so
// the actual in-signal to the shaper ranges from ±0.2 V to ±3.2 V.
//
// Port layout:
//   inputs[0]  : audio in L
//   inputs[1]  : audio in R
//   inputs[2]  : drive  (0 = unity pre-gain, 1 = 16× pre-gain)
//   outputs[0] : audio out L
//   outputs[1] : audio out R

#pragma once

#include <kairos_grid/grid_module.hpp>

#include "sst/waveshapers/QuadWaveshaper.h"

#include <algorithm>
#include <cmath>

namespace kairos_grid::surge {

class SurgeWaveshaperModule : public GridModule {
    static constexpr int   kLanes    = 4;
    static constexpr float k_ln16    = 2.772588722f; // ln(16)
    static constexpr float k_in_norm = 0.2f;         // ±5 V → ±1 V
    static constexpr float k_out_scl = 5.0f;         // ±1 V → ±5 V

    sst::waveshapers::WaveshaperType      type_;
    sst::waveshapers::QuadWaveshaperPtr   fn_l_ = nullptr;
    sst::waveshapers::QuadWaveshaperPtr   fn_r_ = nullptr;
    sst::waveshapers::QuadWaveshaperState state_l_{};
    sst::waveshapers::QuadWaveshaperState state_r_{};

    float in_l_[kLanes]{};
    float in_r_[kLanes]{};
    float drv_[kLanes]{};
    float out_l_[kLanes]{};
    float out_r_[kLanes]{};
    int   pos_ = 0;

  public:
    explicit SurgeWaveshaperModule(sst::waveshapers::WaveshaperType t)
        : GridModule(3, 2), type_(t) {}

    void prepare(const GridProcessArgs&) override {
        fn_l_ = sst::waveshapers::GetQuadWaveshaper(type_);
        fn_r_ = sst::waveshapers::GetQuadWaveshaper(type_);

        float R[sst::waveshapers::n_waveshaper_registers]{};
        sst::waveshapers::initializeWaveshaperRegister(type_, R);
        for (int i = 0; i < sst::waveshapers::n_waveshaper_registers; ++i) {
            state_l_.R[i] = _mm_set1_ps(R[i]);
            state_r_.R[i] = _mm_set1_ps(R[i]);
        }
        state_l_.init = _mm_setzero_ps();
        state_r_.init = _mm_setzero_ps();

        for (int i = 0; i < kLanes; ++i) {
            in_l_[i]  = 0.f;
            in_r_[i]  = 0.f;
            drv_[i]   = 1.f;
            out_l_[i] = 0.f;
            out_r_[i] = 0.f;
        }
        pos_ = 0;
    }

    void process(const GridProcessArgs&) override {
        const float drive_v = std::clamp(inputs[2].voltage, 0.f, 1.f);
        const float drive   = std::exp(drive_v * k_ln16); // 1× – 16×

        // Emit buffered output (4-sample latency relative to corresponding input)
        outputs[0].voltage = out_l_[pos_] * k_out_scl;
        outputs[1].voltage = out_r_[pos_] * k_out_scl;

        // Accumulate current input, normalized to ±1 V for Surge convention
        in_l_[pos_] = inputs[0].voltage * k_in_norm;
        in_r_[pos_] = inputs[1].voltage * k_in_norm;
        drv_[pos_]  = drive;

        if (++pos_ == kLanes) {
            pos_            = 0;
            const __m128 vl = _mm_loadu_ps(in_l_);
            const __m128 vr = _mm_loadu_ps(in_r_);
            const __m128 dv = _mm_loadu_ps(drv_);
            if (fn_l_)
                _mm_storeu_ps(out_l_, fn_l_(&state_l_, vl, dv));
            if (fn_r_)
                _mm_storeu_ps(out_r_, fn_r_(&state_r_, vr, dv));
        }
    }
};

} // namespace kairos_grid::surge
