// SPDX-License-Identifier: GPL-3.0-or-later
// wdf — wave digital filter circuit models; physically accurate nonlinear modules.
//
// Uses chowdsp_wdf (MIT) compile-time wdft topology: circuit elements and their
// connections are encoded in C++ template types at class scope.  No vtable
// overhead; per-sample evaluation is 4 lines in process().
//
// All modules follow the kairos-grid GridModule port convention:
//   inputs[0]  — audio in
//   inputs[1]  — drive (linear gain applied to input before the circuit; 1.0 = unity)
//   outputs[0] — audio out
//
// Modules are non-copyable and non-movable: wdft elements store internal
// pointers to sibling members; moving the owning object would dangle them.
//
// Registry keys (KAIROS_GRID_BUILD_WDF):
//   "diode-clip"   — DiodeClipModule  — antiparallel 1N4148 pair; symmetric soft clip
//   "diode-half"   — DiodeHalfModule  — single 1N4148; asymmetric rectifying clip

#pragma once

#include <chowdsp_wdf/chowdsp_wdf.h>
#include <kairos_grid/grid_module.hpp>

#include <algorithm>

namespace kairos_grid {

// ---------------------------------------------------------------------------
// Shared circuit constants (1N4148 diode, RC network)
// ---------------------------------------------------------------------------

namespace wdf_detail {
    inline constexpr float kIs        = 2.52e-9f;  // 1N4148 saturation current
    inline constexpr float kVt        = 25.85e-3f; // thermal voltage at room temp
    inline constexpr float kResOhms   = 4700.0f;   // series resistance
    inline constexpr float kCapFarads = 47.0e-9f;  // shunt capacitance
} // namespace wdf_detail

// Convenience aliases — not exported beyond this header.
namespace wdft = chowdsp::wdft;

// ---------------------------------------------------------------------------
// DiodeClipModule — antiparallel 1N4148 pair; symmetric soft clipping
//
// Circuit:  Vs ─── R1 ───[node]─── DiodePairT (root)
//                              │
//                             C1
//                              │
//                             gnd
// Output: voltage(C1)
//
// The DiodePairT implements Werner et al. 2015 antiparallel pair model using
// the Wright Omega approximation — no Newton iteration, guaranteed convergence.
// ---------------------------------------------------------------------------

class DiodeClipModule : public GridModule {
  public:
    explicit DiodeClipModule() : GridModule(2, 1) {}

    DiodeClipModule(const DiodeClipModule&)            = delete;
    DiodeClipModule& operator=(const DiodeClipModule&) = delete;

    void prepare(const GridProcessArgs& args) override {
        C1_.prepare(args.sample_rate);
        C1_.reset();
        outputs[0].voltage = 0.f;
    }

    void process(const GridProcessArgs&) override {
        const float drive = std::max(0.f, inputs[1].voltage);
        Vs_.setVoltage(inputs[0].voltage * drive);
        dp_.incident(I1_.reflected());
        outputs[0].voltage = wdft::voltage<float>(C1_);
        I1_.incident(dp_.reflected());
    }

  private:
    wdft::ResistiveVoltageSourceT<float> Vs_;
    wdft::ResistorT<float>               R1_{wdf_detail::kResOhms};
    wdft::CapacitorT<float>              C1_{wdf_detail::kCapFarads, 48000.f};

    decltype(wdft::makeSeries<float>(Vs_, R1_))   S1_ = wdft::makeSeries<float>(Vs_, R1_);
    decltype(wdft::makeParallel<float>(S1_, C1_)) P1_ = wdft::makeParallel<float>(S1_, C1_);
    decltype(wdft::makeInverter<float>(P1_))      I1_ = wdft::makeInverter<float>(P1_);
    wdft::DiodePairT<float, decltype(I1_)>        dp_{I1_, wdf_detail::kIs, wdf_detail::kVt};
};

// ---------------------------------------------------------------------------
// DiodeHalfModule — single 1N4148; asymmetric (half-wave) clipping
//
// Same RC network as DiodeClipModule but with DiodeT instead of DiodePairT.
// Positive half-cycles clip; negative half-cycles pass through the shunt path.
// Character: asymmetric saturation with even harmonics — warmer, more germanium.
// ---------------------------------------------------------------------------

class DiodeHalfModule : public GridModule {
  public:
    explicit DiodeHalfModule() : GridModule(2, 1) {}

    DiodeHalfModule(const DiodeHalfModule&)            = delete;
    DiodeHalfModule& operator=(const DiodeHalfModule&) = delete;

    void prepare(const GridProcessArgs& args) override {
        C1_.prepare(args.sample_rate);
        C1_.reset();
        outputs[0].voltage = 0.f;
    }

    void process(const GridProcessArgs&) override {
        const float drive = std::max(0.f, inputs[1].voltage);
        Vs_.setVoltage(inputs[0].voltage * drive);
        d_.incident(I1_.reflected());
        outputs[0].voltage = wdft::voltage<float>(C1_);
        I1_.incident(d_.reflected());
    }

  private:
    wdft::ResistiveVoltageSourceT<float> Vs_;
    wdft::ResistorT<float>               R1_{wdf_detail::kResOhms};
    wdft::CapacitorT<float>              C1_{wdf_detail::kCapFarads, 48000.f};

    decltype(wdft::makeSeries<float>(Vs_, R1_))   S1_ = wdft::makeSeries<float>(Vs_, R1_);
    decltype(wdft::makeParallel<float>(S1_, C1_)) P1_ = wdft::makeParallel<float>(S1_, C1_);
    decltype(wdft::makeInverter<float>(P1_))      I1_ = wdft::makeInverter<float>(P1_);
    wdft::DiodeT<float, decltype(I1_)>            d_{I1_, wdf_detail::kIs, wdf_detail::kVt};
};

} // namespace kairos_grid
