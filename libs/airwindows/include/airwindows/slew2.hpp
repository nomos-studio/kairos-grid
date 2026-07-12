// SPDX-FileCopyrightText: Chris Johnson <airwindows@airwindows.com>
// SPDX-License-Identifier: MIT
//
// Slew2 — 2× oversampled hard slew-rate limiter from the Airwindows plugin
// collection.
//
// Limits the maximum rate-of-change of the signal.  Internally works at 2×
// the nominal sample rate by computing an interpolated halfway point and
// applying the clamp to both halves independently.  Post-processing uses a
// two-path IIR antialiaser (ping-pong between states A and B) to suppress
// aliasing from the hard limiting.
//
// Constants:
//   decay    = Catalan's constant ≈ 0.9159655941772190
//   highTweak = sqrt(2)-1 ≈ 0.04142135623730951
//
// Parameter A (slew ceiling, [0, 1]):
//   0 = transparent (threshold → ∞)
//   1 = maximum slewing (threshold → 0)
//
// Usage:
//   Slew2Channel ch;
//   ch.prepare(sampleRate);
//   double out = ch.process(in, A);
//
// Audio range: [-1, 1].  Pass audio signals directly; no external scaling.

#pragma once

#include <cmath>

namespace airwindows {

struct Slew2Channel {
    // 4-sample history for the 2× oversampling interpolator
    double last3{}, last2{}, last1{};
    // Internal state
    double halfway{}, halfDry{}, halfDiff{};
    double A{}, B{}, C{};
    double dry{}, diff{}, prevDiff{};
    double lastSample{};
    bool   flip{false};

    static constexpr double kDecay     = 0.915965594177219015;
    static constexpr double kHighTweak = 0.0414213562373095048801688;

    // Pre-computed 1/overallscale = 44100 / (2 * sampleRate)
    double inv_overall_scale{};

    void prepare(double sampleRate) noexcept { inv_overall_scale = 44100.0 / (2.0 * sampleRate); }

    double process(double x, double param_a) noexcept {
        const double threshold = std::pow(1.0 - param_a, 4.0) * inv_overall_scale;

        dry = x;

        // Interpolated halfway point (2× oversampled first virtual sample)
        halfDry = halfway = (x + last1 + ((-last2 + last3) * kHighTweak)) / 2.0;
        last3             = last2;
        last2             = last1;
        last1             = x;

        // --- First half-sample ---
        double clamp = halfway - halfDry;
        if (clamp > threshold)
            halfway = lastSample + threshold;
        if (-clamp > threshold)
            halfway = lastSample - threshold;
        lastSample = halfway;

        C = halfway - halfDry;
        if (flip) {
            A *= kDecay;
            B *= kDecay;
            A += C;
            B -= C;
            C = A;
        } else {
            B *= kDecay;
            A *= kDecay;
            B += C;
            A -= C;
            C = B;
        }
        halfDiff = C * kDecay;
        flip     = !flip;

        // --- Second half-sample (the actual input sample x) ---
        clamp = x - lastSample;
        if (clamp > threshold)
            x = lastSample + threshold;
        if (-clamp > threshold)
            x = lastSample - threshold;
        lastSample = x;

        C = x - dry;
        if (flip) {
            A *= kDecay;
            B *= kDecay;
            A += C;
            B -= C;
            C = A;
        } else {
            B *= kDecay;
            A *= kDecay;
            B += C;
            A -= C;
            C = B;
        }
        diff = C * kDecay;
        flip = !flip;

        x        = dry + (diff + halfDiff + prevDiff) / 0.734;
        prevDiff = diff / 2.0;

        return x;
    }
};

} // namespace airwindows
