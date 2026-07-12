// SPDX-FileCopyrightText: Chris Johnson <airwindows@airwindows.com>
// SPDX-License-Identifier: MIT
//
// Desk — tape desk saturation from the Airwindows plugin collection.
//
// Per-channel DSP state with sample-rate-derived coefficients.  The algorithm
// models slew-rate limiting (sin-based soft clip on transients), a first-order
// comb interaction that adds subtle high-frequency texture, and a final sin
// drive stage that rounds the top end.  No user parameters — all coefficients
// follow from sample rate.
//
// Usage:
//   DeskChannel ch;
//   ch.prepare(sampleRate);
//   double out = ch.process(in);
//
// Audio range: [-1, 1].  Pass audio signals directly; no external scaling.

#pragma once

#include <cmath>

namespace airwindows {

struct DeskChannel {
    double lastSample{};
    double lastOutSample{};
    double lastSlew{};

    // Sample-rate-derived coefficients — set by prepare().
    double slewgain{};
    double prevslew{};
    double balanceB{};
    double balanceA{};

    static constexpr double kGain = 0.135;
    static constexpr double kPi_2 = 1.5707963267948966;

    void prepare(double sampleRate) noexcept {
        const double os = sampleRate / 44100.0;
        slewgain        = 0.208 * os;
        prevslew        = 0.333 * os;
        balanceB        = 0.0001 / os;
        balanceA        = 1.0 - balanceB;
    }

    double process(double x) noexcept {
        const double drySample = x;

        double slew = x - lastSample;
        lastSample  = x;

        // Sin-based slew rate limiter
        double br = std::abs(slew * slewgain);
        if (br > kPi_2)
            br = 1.0;
        else
            br = std::sin(br);
        slew = (slew > 0.0) ? br / slewgain : -(br / slewgain);

        // First-order comb blend
        x             = lastOutSample * balanceA + lastSample * balanceB + slew;
        lastOutSample = x;

        // Comb interaction: modulate slew history by product of dry samples
        double comb = std::abs(drySample * lastSample);
        if (comb > 1.0)
            comb = 1.0;
        x -= lastSlew * comb * prevslew;
        lastSlew = slew;

        // Sin drive stage
        x *= kGain;
        br = std::abs(x);
        if (br > kPi_2)
            br = 1.0;
        else
            br = std::sin(br);
        x = (x > 0.0) ? br : -br;
        x /= kGain;

        return x;
    }
};

} // namespace airwindows
