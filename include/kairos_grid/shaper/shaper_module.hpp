// SPDX-License-Identifier: GPL-3.0-or-later
// shaper — unified memoryless nonlinearity primitive with CV-modulatable ports.
//
// Three analytic curves selectable at patch time:
//   Tanh     — soft saturation; shape morphs toward hard clip (0=tanh, 1=hard)
//   Fold     — sine wavefold; shape scales fold depth exponentially (0=gentle, 1=4× deep)
//   Quantize — bitcrush; shape controls bit depth (0=12-bit clean, 1=2-bit heavy)
//
// Ports (5 inputs, 2 outputs):
//   inputs[0]  in-l   — audio left  [-1, 1]
//   inputs[1]  in-r   — audio right [-1, 1]
//   inputs[2]  drive  — pre-gain CV [0, 1]; 0=unity (1×), 1=16× (exponential taper)
//   inputs[3]  shape  — curve-specific CV [0, 1] (see curve semantics above)
//   inputs[4]  mix    — dry/wet blend CV [0, 1]; 0=dry, 1=fully processed
//   outputs[0] out-l
//   outputs[1] out-r
//
// ADAA (anti-derivative anti-aliasing) is applied to Tanh and Fold.  Quantize
// is intentionally raw — aliasing is a defining part of its character.
//
// Oversampling is NOT baked in.  Per the shaper archetype, OS is a per-instance
// concern: Fold benefits from OS to suppress alias; Quantize wants none.
// Apply external oversampling when alias taming matters for Fold.
//
// Table-backed curve (peek into a transfer-curve buffer) is deferred until the
// peek primitive lands; it will extend this enum without changing the port surface.

#pragma once

#include <kairos_grid/grid_module.hpp>

#include <algorithm>
#include <cmath>

namespace kairos_grid {

enum class ShaperCurve { Tanh, Fold, Quantize };

class ShaperModule : public GridModule {
  public:
    explicit ShaperModule(ShaperCurve curve) : GridModule(5, 2), curve_(curve) {}

    void prepare(const GridProcessArgs&) override {
        prev_l_ = 0.f;
        prev_r_ = 0.f;
    }

    void process(const GridProcessArgs&) override {
        const float drive = std::clamp(inputs[2].voltage, 0.f, 1.f);
        const float shape = std::clamp(inputs[3].voltage, 0.f, 1.f);
        const float mix   = std::clamp(inputs[4].voltage, 0.f, 1.f);

        const float dry_l = inputs[0].voltage;
        const float dry_r = inputs[1].voltage;

        float wet_l, wet_r;

        if (curve_ == ShaperCurve::Quantize) {
            // Quantize: hard-clip post-drive, then apply bit reduction.  No ADAA.
            const float gain = std::exp(drive * k_ln16);
            const float xl   = std::clamp(dry_l * gain, -1.f, 1.f);
            const float xr   = std::clamp(dry_r * gain, -1.f, 1.f);
            // shape=0 → 12-bit (4096 levels), shape=1 → 2-bit (4 levels)
            const float levels = std::pow(2.f, 12.f - shape * 10.f);
            wet_l              = std::round(xl * levels) / levels;
            wet_r              = std::round(xr * levels) / levels;
        } else {
            // Tanh / Fold: ADAA, tracking prev in the post-gain domain.
            float gain = std::exp(drive * k_ln16);
            if (curve_ == ShaperCurve::Fold)
                gain *= std::exp(shape * k_ln4); // 1× to 4× fold depth on top of drive

            const float xl = dry_l * gain;
            const float xr = dry_r * gain;

            // For Tanh, shape is passed so F_antideriv can blend the hard-clip term.
            // For Fold, shape was already baked into the pre-gain above; pass 0.f so
            // the ADAA helper uses the pure fold antiderivative.
            const float adaa_shape = (curve_ == ShaperCurve::Tanh) ? shape : 0.f;

            wet_l = adaa(xl, prev_l_, adaa_shape);
            wet_r = adaa(xr, prev_r_, adaa_shape);

            prev_l_ = xl;
            prev_r_ = xr;
        }

        outputs[0].voltage = dry_l + mix * (wet_l - dry_l);
        outputs[1].voltage = dry_r + mix * (wet_r - dry_r);
    }

  private:
    ShaperCurve curve_;
    float       prev_l_ = 0.f;
    float       prev_r_ = 0.f;

    // ---------------------------------------------------------------------------
    // Direct sample evaluation — used as ADAA fallback when |x - prev| < epsilon.
    // shape is only used for Tanh; Fold receives shape=0 (depth already in gain).
    // ---------------------------------------------------------------------------

    float f_sample(float x, float shape) const noexcept {
        if (curve_ == ShaperCurve::Tanh) {
            const float tanh_x = std::tanh(x);
            const float hard_x = std::clamp(x, -1.f, 1.f);
            return (1.f - shape) * tanh_x + shape * hard_x;
        }
        // Fold
        return std::sin(k_pi_half * x);
    }

    // ---------------------------------------------------------------------------
    // Antiderivative F(x).  Must satisfy F'(x) = f_sample(x, shape).
    // ---------------------------------------------------------------------------

    float F_antideriv(float x, float shape) const noexcept {
        if (curve_ == ShaperCurve::Tanh) {
            // F_tanh(x) = log(cosh(x)); stable as |x|−log(2) for |x|>10.
            const float ax          = std::abs(x);
            const float f_tanh_anti = (ax > 10.f ? ax - k_ln2 : std::log(std::cosh(x)));
            // F_hard(x): x²/2 for |x|≤1; |x|−½ for |x|>1.
            const float f_hard_anti = (ax <= 1.f ? 0.5f * x * x : ax - 0.5f);
            return (1.f - shape) * f_tanh_anti + shape * f_hard_anti;
        }
        // Fold: F(x) = −(2/π)·cos(π/2·x).
        return k_neg2_over_pi * std::cos(k_pi_half * x);
    }

    // ---------------------------------------------------------------------------
    // ADAA: mean-value integral.  Falls back to f_sample when |dx| < epsilon.
    // ---------------------------------------------------------------------------

    float adaa(float x, float prev, float shape) const noexcept {
        const float dx = x - prev;
        if (std::abs(dx) < 1e-5f)
            return f_sample(x, shape);
        return (F_antideriv(x, shape) - F_antideriv(prev, shape)) / dx;
    }

    static constexpr float k_ln16         = 2.772588722f;  // ln(16)
    static constexpr float k_ln4          = 1.386294361f;  // ln(4)
    static constexpr float k_ln2          = 0.693147181f;  // ln(2)
    static constexpr float k_pi_half      = 1.570796327f;  // π/2
    static constexpr float k_neg2_over_pi = -0.636619772f; // −2/π
};

} // namespace kairos_grid
