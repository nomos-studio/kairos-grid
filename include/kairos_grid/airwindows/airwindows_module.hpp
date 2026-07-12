// SPDX-FileCopyrightText: Thomas Rodgers
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Airwindows DSP algorithms wrapped as GridModules.
//
// Three modules:
//
//   DeskModule  ("aw-desk")
//     No parameters.  Stereo tape-desk saturation using Airwindows Desk.
//     Ports: in-l(0), in-r(1) → out-l(0), out-r(1)
//
//   SlewModule  ("aw-slew")
//     One parameter: slew ceiling (0 = transparent, 1 = maximum slewing).
//     Ports: in-l(0), in-r(1), slew(2, [0,1]V) → out-l(0), out-r(1)
//
//   SpiralModule  ("aw-spiral")
//     Two parameters: drive and wet.  Spiral saturation + alternating HPF.
//     drive → Airwindows A param: 0V = silence, 0.5V ≈ unity gain, 1V = 4×.
//     wet   → Airwindows E param: 0V = dry, 1V = fully wet.
//     HPF amount (B), presence (C), and output (D) are hardcoded.
//     Ports: in-l(0), in-r(1), drive(2, [0,1]V), wet(3, [0,1]V)
//            → out-l(0), out-r(1)
//
// Voltage convention: grid audio is [-1, 1] V normalised full-scale.
// All three algorithms expect ±1 audio and are applied directly — no scaling.
// CV/param inputs are 0..1 V and are clamped to [0, 1].

#pragma once

#include <kairos_grid/grid_module.hpp>

#include <airwindows/desk.hpp>
#include <airwindows/slew2.hpp>
#include <airwindows/spiral2.hpp>

#include <algorithm>
#include <cmath>

namespace kairos_grid::airwindows {

// ---------------------------------------------------------------------------
// DeskModule — tape desk saturation, no parameters
// ---------------------------------------------------------------------------

class DeskModule : public GridModule {
    ::airwindows::DeskChannel l_{}, r_{};

  public:
    DeskModule() : GridModule(2, 2) {}

    void prepare(const GridProcessArgs& args) override {
        l_.prepare(static_cast<double>(args.sample_rate));
        r_.prepare(static_cast<double>(args.sample_rate));
    }

    void process(const GridProcessArgs&) override {
        outputs[0].voltage = static_cast<float>(l_.process(static_cast<double>(inputs[0].voltage)));
        outputs[1].voltage = static_cast<float>(r_.process(static_cast<double>(inputs[1].voltage)));
    }
};

// ---------------------------------------------------------------------------
// SlewModule — 2× oversampled slew-rate limiter
// ---------------------------------------------------------------------------

class SlewModule : public GridModule {
    ::airwindows::Slew2Channel l_{}, r_{};

  public:
    SlewModule() : GridModule(3, 2) {}

    void prepare(const GridProcessArgs& args) override {
        l_.prepare(static_cast<double>(args.sample_rate));
        r_.prepare(static_cast<double>(args.sample_rate));
    }

    void process(const GridProcessArgs&) override {
        const double slew = std::clamp(static_cast<double>(inputs[2].voltage), 0.0, 1.0);
        outputs[0].voltage =
            static_cast<float>(l_.process(static_cast<double>(inputs[0].voltage), slew));
        outputs[1].voltage =
            static_cast<float>(r_.process(static_cast<double>(inputs[1].voltage), slew));
    }
};

// ---------------------------------------------------------------------------
// SpiralModule — alternating-IIR HPF + spiral saturation
// ---------------------------------------------------------------------------

class SpiralModule : public GridModule {
    ::airwindows::Spiral2Channel l_{}, r_{};

    double sample_rate_ = 48000.0;
    double iir_amount_  = 0.0;

    // Hardcoded character parameters
    static constexpr double kHPF      = 0.1; // B: gentle DC-block
    static constexpr double kPresence = 0.0; // C: no presence blend
    static constexpr double kOutput   = 1.0; // D: unity output

  public:
    SpiralModule() : GridModule(4, 2) {}

    void prepare(const GridProcessArgs& args) override {
        sample_rate_ = static_cast<double>(args.sample_rate);
        iir_amount_  = std::pow(kHPF, 3.0) / (sample_rate_ / 44100.0);
    }

    void process(const GridProcessArgs&) override {
        const double drive = std::clamp(static_cast<double>(inputs[2].voltage), 0.0, 1.0);
        const double wet   = std::clamp(static_cast<double>(inputs[3].voltage), 0.0, 1.0);
        const double gain  = std::pow(drive * 2.0, 2.0);

        outputs[0].voltage = static_cast<float>(l_.process(
            static_cast<double>(inputs[0].voltage), gain, iir_amount_, kPresence, kOutput, wet));
        outputs[1].voltage = static_cast<float>(r_.process(
            static_cast<double>(inputs[1].voltage), gain, iir_amount_, kPresence, kOutput, wet));
    }
};

} // namespace kairos_grid::airwindows
