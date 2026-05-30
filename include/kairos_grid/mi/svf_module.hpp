// SPDX-License-Identifier: GPL-3.0-or-later
// State-variable filter (stmlib ZDF SVF) wrapped as a GridModule.
//
// Inputs:
//   0  in        — audio signal
//   1  freq      — normalised cutoff (0 = 0 Hz, 1 = Nyquist)
//   2  q         — resonance 0-1 → internal Q range [0.5, 16.5]
// Outputs:
//   0  lp        — low-pass
//   1  hp        — high-pass
//   2  bp        — band-pass  (derived from SVF identity: in = lp + r·bp + hp)

#pragma once

#include <kairos_grid/grid_module.hpp>

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wasm-operand-widths"
#  pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <stmlib/dsp/filter.h>
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

namespace kairos_grid::mi {

class SvfModule : public GridModule {
  public:
    SvfModule() : GridModule(3, 3) { svf_.Init(); }

    void process(const GridProcessArgs&) override {
        float freq = inputs[1].voltage;
        float q    = inputs[2].voltage;

        // Clamp to safe ranges for the approximation.
        if (freq < 0.f)   freq = 0.f;
        if (freq > 0.499f) freq = 0.499f;
        float resonance = 0.5f + q * 16.f;

        svf_.set_f_q<stmlib::FREQUENCY_FAST>(freq, resonance);

        float in = inputs[0].voltage;
        float lp, hp;
        svf_.Process<stmlib::FILTER_MODE_LOW_PASS,
                     stmlib::FILTER_MODE_HIGH_PASS>(in, &lp, &hp);

        outputs[0].voltage = lp;
        outputs[1].voltage = hp;
        // SVF identity: in = lp + r·bp + hp  →  bp = (in - lp - hp) / r
        outputs[2].voltage = (in - lp - hp) / svf_.r();
    }

  private:
    stmlib::Svf svf_;
};

} // namespace kairos_grid::mi
