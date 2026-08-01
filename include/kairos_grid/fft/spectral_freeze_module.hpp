// SPDX-License-Identifier: GPL-3.0-or-later
// spectral-freeze — STFT freeze resynthesis.
//
// Observes audio continuously via STFT, latches the current magnitude spectrum
// on the rising edge of a freeze gate, and re-synthesizes that snapshot as a
// continuous audio stream via IFFT with randomized per-hop phases.  The random-
// phase approach produces a cloud/wash texture — the same character as
// Panharmonium or Clouds SPECTRAL mode.
//
// Port surface: 3 inputs, 2 outputs.
//   inputs[0]  in-l     audio input, left channel
//   inputs[1]  in-r     audio input, right channel
//   inputs[2]  freeze   gate >0.5 V latches spectrum; rising edge triggers latch
//   outputs[0] out-l    resynthesized audio, left channel
//   outputs[1] out-r    resynthesized audio, right channel
//
// Behavior:
//   freeze low  → continuous analysis, outputs silent
//   rising edge → latch current magnitude spectrum, reset OLA buffers
//   freeze high → each hop: IFFT with frozen magnitudes + fresh random phases,
//                 overlap-add into output stream (Hann window, 75% overlap)
//   falling edge → synthesis stops, outputs return to silence
//
// Latency: up to one hop (window/4 samples) from freeze trigger to first audio.
//
// Registry types (all behind KAIROS_GRID_BUILD_FFT):
//   "spectral-freeze"      — 1024-sample window (default, ~21 ms at 48 kHz)
//   "spectral-freeze-512"  — 512-sample window  (~10.7 ms)
//   "spectral-freeze-2048" — 2048-sample window (~42.7 ms)
//   "spectral-freeze-4096" — 4096-sample window (~85.3 ms)

#pragma once

#include <kairos_grid/grid_module.hpp>
#include <kiss_fft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace kairos_grid {

class SpectralFreezeModule : public GridModule {
  public:
    explicit SpectralFreezeModule(std::size_t window = 1024)
        : GridModule(3, 2), window_(window), hop_size_(window / 4), hann_(window) {
        const float denom = static_cast<float>(window_ > 1u ? window_ - 1u : 1u);
        for (std::size_t i = 0; i < window_; ++i)
            hann_[i] = 0.5f * (1.f - std::cos(kTwoPi * static_cast<float>(i) / denom));

        const int N = static_cast<int>(window_);
        for (int c = 0; c < 2; ++c) {
            auto& ch = ch_[c];
            ch.ring.assign(window_, 0.f);
            ch.in_cpx.resize(window_);
            ch.fwd_out.resize(window_);
            ch.mag.assign(window_ / 2 + 1, 0.f);
            ch.frozen_mag.assign(window_ / 2 + 1, 0.f);
            ch.synth_cpx.resize(window_);
            ch.ifft_out.resize(window_);
            ch.acc.assign(2 * window_, 0.f);
            for (auto& cx : ch.in_cpx) {
                cx.r = 0.f;
                cx.i = 0.f;
            }
            ch.fwd_cfg = kiss_fft_alloc(N, 0 /* forward */, nullptr, nullptr);
            ch.inv_cfg = kiss_fft_alloc(N, 1 /* inverse */, nullptr, nullptr);
            ch.rng     = static_cast<uint64_t>(c + 1) * 0xdeadbeefcafe1234ULL;
        }
    }

    ~SpectralFreezeModule() override {
        for (auto& ch : ch_) {
            if (ch.fwd_cfg)
                kiss_fft_free(ch.fwd_cfg);
            if (ch.inv_cfg)
                kiss_fft_free(ch.inv_cfg);
        }
    }

    SpectralFreezeModule(const SpectralFreezeModule&)            = delete;
    SpectralFreezeModule& operator=(const SpectralFreezeModule&) = delete;

    void prepare(const GridProcessArgs&) override {
        is_frozen_   = false;
        prev_freeze_ = false;
        for (auto& out : outputs)
            out.voltage = 0.f;
        for (auto& ch : ch_) {
            std::fill(ch.ring.begin(), ch.ring.end(), 0.f);
            std::fill(ch.mag.begin(), ch.mag.end(), 0.f);
            std::fill(ch.frozen_mag.begin(), ch.frozen_mag.end(), 0.f);
            std::fill(ch.acc.begin(), ch.acc.end(), 0.f);
            ch.wpos      = 0;
            ch.hop_count = 0;
            ch.acc_wpos  = 0;
            ch.acc_rpos  = 0;
        }
    }

    void process(const GridProcessArgs&) override {
        const bool curr_freeze = (inputs[2].voltage > 0.5f);
        const bool rising_edge = curr_freeze && !prev_freeze_;

        if (rising_edge)
            on_freeze_trigger();
        if (!curr_freeze)
            is_frozen_ = false;
        prev_freeze_ = curr_freeze;

        for (int c = 0; c < 2; ++c) {
            auto& ch = ch_[c];

            // Feed sample into analysis ring buffer.
            ch.ring[ch.wpos] = inputs[c].voltage;
            ch.wpos          = (ch.wpos + 1) % window_;

            if (++ch.hop_count >= hop_size_) {
                ch.hop_count = 0;
                run_analysis_hop(ch);
                if (is_frozen_)
                    do_synthesis_hop(ch);
            }

            if (is_frozen_) {
                const std::size_t rpos = ch.acc_rpos % (2 * window_);
                outputs[c].voltage     = ch.acc[rpos];
                ch.acc[rpos]           = 0.f;
                ++ch.acc_rpos;
            } else {
                outputs[c].voltage = 0.f;
            }
        }
    }

    std::size_t window() const noexcept { return window_; }
    std::size_t hop_size() const noexcept { return hop_size_; }

  private:
    static constexpr float kTwoPi = 6.28318530717958647692f;

    struct ChanState {
        // Analysis
        std::vector<float>        ring;
        std::vector<kiss_fft_cpx> in_cpx;
        std::vector<kiss_fft_cpx> fwd_out;
        std::vector<float>        mag;
        std::vector<float>        frozen_mag;
        kiss_fft_cfg              fwd_cfg{nullptr};
        std::size_t               wpos{0};
        std::size_t               hop_count{0};

        // Synthesis
        std::vector<kiss_fft_cpx> synth_cpx;
        std::vector<kiss_fft_cpx> ifft_out;
        std::vector<float>        acc; // OLA accumulation, size 2*window
        kiss_fft_cfg              inv_cfg{nullptr};
        std::size_t               acc_wpos{0};
        std::size_t               acc_rpos{0};
        uint64_t                  rng{1};
    };

    std::size_t              window_;
    std::size_t              hop_size_;
    std::vector<float>       hann_;
    std::array<ChanState, 2> ch_;
    bool                     is_frozen_{false};
    bool                     prev_freeze_{false};

    void on_freeze_trigger() {
        is_frozen_ = true;
        for (auto& ch : ch_) {
            std::copy(ch.mag.begin(), ch.mag.end(), ch.frozen_mag.begin());
            std::fill(ch.acc.begin(), ch.acc.end(), 0.f);
            ch.acc_wpos = 0;
            ch.acc_rpos = 0;
        }
    }

    float next_phase(ChanState& ch) noexcept {
        // 64-bit LCG (Knuth); real-time safe, no heap.
        ch.rng          = ch.rng * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto bits = static_cast<uint32_t>(ch.rng >> 33);
        return static_cast<float>(bits) * (kTwoPi / static_cast<float>(0x80000000u));
    }

    void run_analysis_hop(ChanState& ch) {
        for (std::size_t i = 0; i < window_; ++i) {
            const std::size_t idx = (ch.wpos + i) % window_;
            ch.in_cpx[i].r        = ch.ring[idx] * hann_[i];
            ch.in_cpx[i].i        = 0.f;
        }
        kiss_fft(ch.fwd_cfg, ch.in_cpx.data(), ch.fwd_out.data());

        const std::size_t nb = window_ / 2 + 1;
        for (std::size_t k = 0; k < nb; ++k) {
            const float r = ch.fwd_out[k].r, im = ch.fwd_out[k].i;
            ch.mag[k] = std::sqrt(r * r + im * im);
        }
    }

    void do_synthesis_hop(ChanState& ch) {
        const std::size_t nb = window_ / 2 + 1;
        const std::size_t N  = window_;

        // Build Hermitian spectrum: frozen magnitudes + random phases.
        // DC and Nyquist bins are real-only (no imaginary part).
        ch.synth_cpx[0]      = {ch.frozen_mag[0], 0.f};
        ch.synth_cpx[nb - 1] = {ch.frozen_mag[nb - 1], 0.f};

        for (std::size_t k = 1; k < nb - 1; ++k) {
            const float phi     = next_phase(ch);
            const float cs      = ch.frozen_mag[k] * std::cos(phi);
            const float sn      = ch.frozen_mag[k] * std::sin(phi);
            ch.synth_cpx[k]     = {cs, sn};
            ch.synth_cpx[N - k] = {cs, -sn}; // conjugate for Hermitian symmetry
        }

        // IFFT (KissFFT inverse is unnormalized — scales by N).
        kiss_fft(ch.inv_cfg, ch.synth_cpx.data(), ch.ifft_out.data());

        // Apply synthesis Hann window, normalize by N, overlap-add into acc.
        // Scale 1/N undoes the KissFFT factor; Hann OLA at 75% overlap sums to ~2,
        // yielding approximately unity gain for typical broadband spectra.
        const float scale = 1.f / static_cast<float>(N);
        for (std::size_t n = 0; n < N; ++n) {
            ch.acc[(ch.acc_wpos + n) % (2 * N)] += ch.ifft_out[n].r * hann_[n] * scale;
        }
        ch.acc_wpos = (ch.acc_wpos + hop_size_) % (2 * N);
    }
};

} // namespace kairos_grid
