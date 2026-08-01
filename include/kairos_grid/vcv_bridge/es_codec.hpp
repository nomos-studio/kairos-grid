// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// ES-8CV and ES-5 block-rate codec.
//
// ES-8CV packs 8 × 12-bit CV values into 4 f32 audio samples using Q23 encoding.
// Two 12-bit values share one 24-bit signed integer; the integer maps to [-1.0, 1.0)
// exactly representable in f32.  All 12-bit input pairs round-trip without loss.
//
// ES-5 packs 8 gate bits into one f32 sample.  Output is in [-1.0, 0.9921875].
// All 256 byte values round-trip without loss.
//
// Both encode/decode functions are pure (no side effects, no allocations).
// They operate on one block's worth of state — call once per audio block, not per sample.
//
// CV value convention (caller's responsibility, not enforced here):
//   [0, 4095] — unsigned 12-bit; caller maps to volts (e.g. 0→-5V, 2048→0V, 4095→+5V)
//
// Gate bit convention:
//   bit n of the uint8_t bitmask = gate channel n (1 = high, 0 = low)

namespace kairos_grid::vcv_bridge {

// ES-8CV encode: pack cv[0..7] (12-bit, range [0, 4095]) into out[0..3] (f32, [-1, 1)).
//
// Packing: pairs (cv[0],cv[1]) → out[0], (cv[2],cv[3]) → out[1], etc.
// Bit layout per pair:  word = (cv_hi << 12) | cv_lo  (24-bit unsigned)
// f32 encoding:  out = (float(word) - 8388608.0f) / 8388608.0f
//
// Lossless: all 24-bit words in [0, 16777215] map to unique f32 values in [-1, 1).
// f32 has a 24-bit mantissa; n/2^23 for integer n in [-2^23, 2^23-1] is always exact.
inline void es8cv_encode(const uint16_t cv[8], float out[4]) noexcept {
    constexpr float kInvScale = 1.0f / 8388608.0f; // 1 / 2^23
    for (int i = 0; i < 4; ++i) {
        const uint32_t word = (static_cast<uint32_t>(cv[2 * i] & 0xFFFu) << 12) |
                              static_cast<uint32_t>(cv[2 * i + 1] & 0xFFFu);
        out[i] = (static_cast<float>(static_cast<int32_t>(word)) - 8388608.0f) * kInvScale;
    }
}

// ES-8CV decode: unpack in[0..3] (f32) into cv[0..7] (12-bit, range [0, 4095]).
//
// No rounding: all valid encoded values are exactly representable in f32, so
// multiplying back by 2^23 recovers the integer word without error.
inline void es8cv_decode(const float in[4], uint16_t cv[8]) noexcept {
    for (int i = 0; i < 4; ++i) {
        const int32_t word = static_cast<int32_t>(in[i] * 8388608.0f) + 8388608;
        cv[2 * i]          = static_cast<uint16_t>((word >> 12) & 0xFFF);
        cv[2 * i + 1]      = static_cast<uint16_t>(word & 0xFFF);
    }
}

// ES-5 encode: pack 8 gate bits (bitmask) into one f32 sample in [-1.0, 0.9921875].
//
// Encoding: f = (float(gates) - 128.0f) / 128.0f
// Lossless: (gates - 128) / 128 = (gates - 128) × 2^-7, always exact in f32.
inline float es5_encode(uint8_t gates) noexcept {
    return (static_cast<float>(gates) - 128.0f) * (1.0f / 128.0f);
}

// ES-5 decode: recover 8 gate bits from one f32 sample.
inline uint8_t es5_decode(float f) noexcept {
    const int v = static_cast<int>(f * 128.0f + 128.5f);
    if (v < 0)
        return 0u;
    if (v > 255)
        return 255u;
    return static_cast<uint8_t>(v);
}

} // namespace kairos_grid::vcv_bridge
