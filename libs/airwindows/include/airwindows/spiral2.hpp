// SPDX-FileCopyrightText: Chris Johnson <airwindows@airwindows.com>
// SPDX-License-Identifier: MIT
//
// Spiral2 — alternating-IIR highpass + sin(x·|x|)/|x| spiral saturation from
// the Airwindows plugin collection.
//
// The algorithm applies a one-pole alternating highpass (IIR A/B ping-pong)
// to remove DC, then processes the signal through the Spiral saturation law:
//   y = sin(x · |x|) / |x|
// which is a soft clipper with a distinctive character (more gradual onset than
// tanh, asymmetric harmonic content at higher drives).  An optional presence
// blend mixes in an inter-sample cross-modulated version for added texture.
//
// Five parameters (all [0, 1]):
//   A — input gain:  gain = pow(A * 2, 2)  →  0=silence, 0.5=unity, 1=4×
//   B — HPF amount:  iirAmount = pow(B, 3) / (sampleRate/44100)
//   C — presence:    blends in cross-modulated saturation tone
//   D — output:      output level (1.0 = unity)
//   E — wet:         dry/wet blend (0=dry, 1=fully wet)
//
// Usage (per-channel):
//   Spiral2Channel ch;
//   double out = ch.process(in, gain, iirAmount, presence, output, wet);
//
// The Spiral2Channel may be used independently per channel; since flip is
// toggled once per process() call and both channels are processed every sample,
// two Spiral2Channel instances (L and R) remain in sync when called in order.
//
// Audio range: [-1, 1].  Pass audio signals directly; no external scaling.

#pragma once

#include <cmath>

namespace airwindows {

struct Spiral2Channel {
    double iirA{};
    double iirB{};
    double prevSample{}; // pre-gain dry from previous call
    bool   flip{false};

    double process(double x, double gain, double iirAmount, double presence, double output,
                   double wet) noexcept {
        const double drySample = x;

        if (gain != 1.0) {
            x *= gain;
            prevSample *= gain; // scale stored prev by current gain
        }

        // Alternating IIR highpass — removes DC, reduces muddiness
        if (flip) {
            iirA = iirA * (1.0 - iirAmount) + x * iirAmount;
            x -= iirA;
        } else {
            iirB = iirB * (1.0 - iirAmount) + x * iirAmount;
            x -= iirB;
        }

        // Presence: spiral law cross-modulated by previous sample
        const double denom_prev     = (prevSample == 0.0) ? 1.0 : std::abs(prevSample);
        const double presenceSample = std::sin(x * std::abs(prevSample)) / denom_prev;

        // Spiral saturation: sin(x·|x|) / |x|
        const double ax = std::abs(x);
        x               = std::sin(x * ax) / ((ax == 0.0) ? 1.0 : ax);

        if (output < 1.0) {
            x *= output;
        }
        if (presence > 0.0)
            x = x * (1.0 - presence) + presenceSample * presence;
        if (wet < 1.0)
            x = drySample * (1.0 - wet) + x * wet;

        prevSample = drySample;
        flip       = !flip;

        return x;
    }
};

} // namespace airwindows
