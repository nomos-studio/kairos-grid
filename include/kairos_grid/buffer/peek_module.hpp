// SPDX-License-Identifier: GPL-3.0-or-later
// peek — CV-indexed buffer read; one of the two halves of the CV-addressable
// buffer archetype (the other being poke, the write side).
//
// The buffer is a flat float array in heap memory.  Index [0, 1] maps linearly to
// buffer positions [0, N-1].  At index=0: first sample; index=1: last sample.
//
// Interpolation kernel is patch-time (construction argument):
//   None   — floor (nearest-lower); zipper/aliasing character; no extra cost
//   Linear — lerp between adjacent samples; smooth, good for wavetable LFOs
//   Cubic  — Catmull-Rom Hermite; highest quality; ~4× interpolation ops vs linear
//
// Kernel choice is character, per the archetype design: no-interp is where the
// grit lives; cubic is where the smoothness lives.  Neither is "better."
//
// Ports (2 inputs, 2 outputs):
//   inputs[0]  index-l — CV [0, 1]; clamped; left channel buffer read position
//   inputs[1]  index-r — CV [0, 1]; clamped; right channel buffer read position
//   outputs[0] out-l
//   outputs[1] out-r
//
// Buffer is shared between L and R reads.  Pass the same signal to both for mono
// tables; use independent signals for stereo wavetables with per-voice detuning.
//
// buffer() returns a non-const reference; the upcoming poke primitive will write
// into it via this accessor.  All access is assumed audio-thread-only in v1.
//
// Pre-filled buffer variants are registered in the module registry as:
//   "peek-sine"  — one cycle sine    (index 0→0, 0.25→peak, 0.5→0, 0.75→trough)
//   "peek-tri"   — one cycle triangle (index 0→0, 0.25→1, 0.5→0, 0.75→-1)
//   "peek-saw"   — rising sawtooth   (index 0→-1, 1→+1)
// Blank-buffer (default zeroes) variants:
//   "peek"       — linear interp
//   "peek-nn"    — nearest-neighbor
//   "peek-cubic" — cubic

#pragma once

#include <kairos_grid/grid_module.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace kairos_grid {

enum class PeekInterp { None, Linear, Cubic };

class PeekModule : public GridModule {
  public:
    // Standalone: allocates its own buffer.
    explicit PeekModule(PeekInterp interp, std::size_t size = 2048)
        : GridModule(2, 2), buf_ptr_(std::make_shared<std::vector<float>>(size, 0.f)),
          interp_(interp) {}

    // Shared: receives a pre-created buffer (from BufferModule or the buf_id factory).
    PeekModule(PeekInterp interp, std::shared_ptr<std::vector<float>> shared_buf)
        : GridModule(2, 2), buf_ptr_(std::move(shared_buf)), interp_(interp) {}

    std::vector<float>&       buffer() noexcept { return *buf_ptr_; }
    const std::vector<float>& buffer() const noexcept { return *buf_ptr_; }

    void process(const GridProcessArgs&) override {
        outputs[0].voltage = read(std::clamp(inputs[0].voltage, 0.f, 1.f));
        outputs[1].voltage = read(std::clamp(inputs[1].voltage, 0.f, 1.f));
    }

    // ---------------------------------------------------------------------------
    // Static fill helpers — called by registry setup lambdas for pre-filled variants.
    // ---------------------------------------------------------------------------

    static void fill_sine(std::vector<float>& buf) noexcept {
        const std::size_t N     = buf.size();
        const double      twopi = 6.283185307179586;
        for (std::size_t i = 0; i < N; ++i)
            buf[i] = static_cast<float>(
                std::sin(twopi * static_cast<double>(i) / static_cast<double>(N)));
    }

    // Triangle: 0 → 0, 0.25 → +1, 0.5 → 0, 0.75 → -1, 1 → 0.
    static void fill_triangle(std::vector<float>& buf) noexcept {
        const std::size_t N = buf.size();
        for (std::size_t i = 0; i < N; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(N);
            float       v;
            if (t < 0.25f)
                v = 4.f * t;
            else if (t < 0.75f)
                v = 2.f - 4.f * t;
            else
                v = 4.f * t - 4.f;
            buf[i] = v;
        }
    }

    // Rising sawtooth: index 0 → -1, index 1 → +1 (exclusive; wraps).
    static void fill_saw(std::vector<float>& buf) noexcept {
        const std::size_t N = buf.size();
        for (std::size_t i = 0; i < N; ++i)
            buf[i] = 2.f * static_cast<float>(i) / static_cast<float>(N) - 1.f;
    }

  private:
    std::shared_ptr<std::vector<float>> buf_ptr_;
    PeekInterp                          interp_;

    // Convenience alias — keeps read helpers readable.
    const std::vector<float>& buf() const noexcept { return *buf_ptr_; }

    // Map normalised index [0,1] to fractional buffer position [0, N-1], then
    // interpolate.
    float read(float idx) const noexcept {
        const float pos_f = idx * static_cast<float>(buf().size() - 1);
        switch (interp_) {
        case PeekInterp::None:
            return read_nn(pos_f);
        case PeekInterp::Linear:
            return read_linear(pos_f);
        case PeekInterp::Cubic:
            return read_cubic(pos_f);
        }
        return read_linear(pos_f); // unreachable; silences warning
    }

    // Floor (nearest-lower): classic staircase / zipper character.
    float read_nn(float pos_f) const noexcept {
        const std::size_t i = static_cast<std::size_t>(pos_f);
        return buf()[std::min(i, buf().size() - 1)];
    }

    float read_linear(float pos_f) const noexcept {
        const std::size_t i0   = static_cast<std::size_t>(pos_f);
        const std::size_t i1   = std::min(i0 + 1, buf().size() - 1);
        const float       frac = pos_f - static_cast<float>(i0);
        return buf()[i0] + frac * (buf()[i1] - buf()[i0]);
    }

    // Catmull-Rom Hermite cubic.  At the boundaries the two outermost control
    // points are clamped to the nearest valid index so the formula stays consistent
    // at the edges of the buffer.
    float read_cubic(float pos_f) const noexcept {
        const int   N  = static_cast<int>(buf().size());
        const int   i1 = static_cast<int>(pos_f);
        const float f  = pos_f - static_cast<float>(i1);

        auto s = [&](int i) noexcept -> float {
            return buf()[static_cast<std::size_t>(std::clamp(i, 0, N - 1))];
        };

        const float y0 = s(i1 - 1);
        const float y1 = s(i1);
        const float y2 = s(i1 + 1);
        const float y3 = s(i1 + 2);

        const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c = -0.5f * y0 + 0.5f * y2;
        const float d = y1;

        return ((a * f + b) * f + c) * f + d;
    }
};

} // namespace kairos_grid
