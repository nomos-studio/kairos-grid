// SPDX-License-Identifier: GPL-3.0-or-later
// One-pole filter (stmlib ZDF) wrapped as a GridModule.
//
// Inputs:
//   0  in        — audio signal
//   1  freq      — normalised cutoff (0 = 0 Hz, 1 = Nyquist)
// Outputs:
//   0  lp        — low-pass
//   1  hp        — high-pass  (= in - lp, exact)

#pragma once

#include <kairos_grid/grid_module.hpp>

#include <stmlib/dsp/filter.h>

namespace kairos_grid::mi {

class OnePoleModule : public GridModule {
  public:
    OnePoleModule() : GridModule(2, 2) { pole_.Init(); }

    void process(const GridProcessArgs&) override {
        float freq = inputs[1].voltage;
        if (freq < 0.f)    freq = 0.f;
        if (freq > 0.497f) freq = 0.497f;

        pole_.set_f<stmlib::FREQUENCY_DIRTY>(freq);

        float in = inputs[0].voltage;
        float lp = pole_.Process<stmlib::FILTER_MODE_LOW_PASS>(in);

        outputs[0].voltage = lp;
        outputs[1].voltage = in - lp;
    }

  private:
    stmlib::OnePole pole_;
};

} // namespace kairos_grid::mi
