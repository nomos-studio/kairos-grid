// SPDX-License-Identifier: GPL-3.0-or-later
// Anti-derivative anti-aliased (ADAA) waveshaper — four shapes, no oversampling.
//
// ADAA replaces naive per-sample evaluation f(x[n]) with the mean-value
// integral (F(x[n]) - F(x[n-1])) / (x[n] - x[n-1]), where F is the closed-form
// antiderivative of the shaping function f.  This suppresses aliasing caused by
// the discontinuity in f' (for hard clip) or by operating near Nyquist, without
// the cost or latency of oversampling.  Falls back to f(x[n]) when consecutive
// samples are within epsilon (mean-value theorem gives no useful information for
// an infinitesimal interval).
//
// Registered shapes:
//   "ws-hard" — hard clip (±1 ceiling, discontinuous derivative at ±1)
//   "ws-tanh" — tanh soft saturation (smooth, bounded ±1)
//   "ws-soft" — cubic soft clip ((3x-x³)/2 for |x|≤1, ±1 beyond)
//   "ws-fold" — sine wavefold (sin(π/2·x), folds every ±2 units of driven input)
//
// Inputs:
//   0  in-l   — audio left
//   1  in-r   — audio right
//   2  drive  — 0=unity gain, 1=16× pre-gain (exponential taper)
// Outputs:
//   0  out-l
//   1  out-r

#pragma once

#include <kairos_grid/grid_module.hpp>

#include <algorithm>
#include <cmath>

namespace kairos_grid::mi {

class WaveshaperModule : public GridModule {
  public:
    using ShapeFn = float (*)(float);

    WaveshaperModule(ShapeFn f, ShapeFn F) : GridModule(3, 2), f_(f), F_(F) {}

    void prepare(const GridProcessArgs&) override {
        prev_l_ = 0.f;
        prev_r_ = 0.f;
    }

    void process(const GridProcessArgs&) override {
        const float drive = std::clamp(inputs[2].voltage, 0.f, 1.f);
        // Exponential pre-gain: 1× at drive=0, 16× at drive=1.
        const float gain = std::exp(drive * k_ln16);

        const float xl = inputs[0].voltage * gain;
        const float xr = inputs[1].voltage * gain;

        outputs[0].voltage = adaa(xl, prev_l_);
        outputs[1].voltage = adaa(xr, prev_r_);

        prev_l_ = xl;
        prev_r_ = xr;
    }

    // ---------------------------------------------------------------------------
    // Shape functions and their antiderivatives — public so registry lambdas can
    // form function pointers without friendship boilerplate.
    // ---------------------------------------------------------------------------

    static float f_hard(float x) { return std::clamp(x, -1.f, 1.f); }
    static float F_hard(float x) {
        // F(x) = x²/2 for |x|≤1; |x|−½ for |x|>1.  Continuous at ±1 (both give ½).
        const float ax = std::abs(x);
        return ax <= 1.f ? 0.5f * x * x : ax - 0.5f;
    }

    static float f_tanh(float x) { return std::tanh(x); }
    static float F_tanh(float x) {
        // F(x) = log(cosh(x)).  For |x|>10 use |x|−log(2) to avoid cosh overflow.
        const float ax = std::abs(x);
        return ax > 10.f ? ax - k_ln2 : std::log(std::cosh(x));
    }

    static float f_soft(float x) {
        // Cubic: (3x−x³)/2 for |x|≤1, ±1 beyond.
        if (std::abs(x) >= 1.f)
            return x > 0.f ? 1.f : -1.f;
        return (3.f * x - x * x * x) * 0.5f;
    }
    static float F_soft(float x) {
        // F is even (f is odd with F(0)=0): F(x) = 3x²/4−x⁴/8 for |x|≤1.
        // Continuity: F(1)=5/8, so for |x|>1: F(x)=|x|−3/8.
        const float ax = std::abs(x);
        return ax <= 1.f ? 0.75f * x * x - 0.125f * x * x * x * x : ax - 0.375f;
    }

    static float f_fold(float x) { return std::sin(k_pi_half * x); }
    static float F_fold(float x) {
        // F(x) = −(2/π)·cos(π/2·x).
        return k_neg2_over_pi * std::cos(k_pi_half * x);
    }

  private:
    ShapeFn f_;
    ShapeFn F_;
    float   prev_l_ = 0.f;
    float   prev_r_ = 0.f;

    float adaa(float x, float prev) const {
        const float dx = x - prev;
        if (std::abs(dx) < 1e-5f)
            return f_(x);
        return (F_(x) - F_(prev)) / dx;
    }

    static constexpr float k_ln16         = 2.772588722f;  // ln(16)
    static constexpr float k_ln2          = 0.693147181f;  // ln(2)
    static constexpr float k_pi_half      = 1.570796327f;  // π/2
    static constexpr float k_neg2_over_pi = -0.636619772f; // −2/π
};

} // namespace kairos_grid::mi
