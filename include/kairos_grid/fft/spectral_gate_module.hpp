// SPDX-License-Identifier: GPL-3.0-or-later
// spectral-gate — per-bin magnitude gating with original-phase synthesis.
//
// Analyzes audio via STFT, gates individual bins whose magnitude falls below
// a threshold relative to the spectral peak, then resynthesizes using the
// original FFT phases (not random phases).  Transparent character: sounds
// like a precision noise gate applied per frequency band.
//
// Port surface: 4 inputs, 2 outputs.
//   inputs[0]  in-l       audio input, left channel
//   inputs[1]  in-r       audio input, right channel
//   inputs[2]  threshold  [0, 1] — gate threshold relative to peak bin;
//                          0 = gate nothing (all bins pass), 1 = only peak bin
//   inputs[3]  floor      [0, 1] — attenuation of gated bins;
//                          0 = hard gate (fully mute), 1 = no reduction (pass-through)
//   outputs[0] out-l      gated audio, left channel
//   outputs[1] out-r      gated audio, right channel
//
// Gate logic (per hop, per bin k):
//   gate_threshold = max_magnitude * threshold
//   scale[k] = 1.0          if mag[k] >= gate_threshold
//             = floor        otherwise
//   synth_cpx[k] = fwd_out[k] * scale[k]   (original phase preserved)
//
// Phase model: original FFT phases — result is spectrally filtered audio, not
// a wash.  Distinguish from spectral-smear/spectral-freeze which randomize phases.
//
// Overlap-add: Hann window applied pre-FFT and post-IFFT; hop = window/4 (75% overlap).
// IFFT normalized by 1/N; OLA accumulation buffer is 2*window samples circular.
//
// Registry types (all behind KAIROS_GRID_BUILD_FFT):
//   "spectral-gate"       — 1024-sample window (default, ~21 ms at 48 kHz)
//   "spectral-gate-512"   — 512-sample window
//   "spectral-gate-2048"  — 2048-sample window
//   "spectral-gate-4096"  — 4096-sample window

#pragma once

#include <kairos_grid/grid_module.hpp>
#include <kiss_fft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace kairos_grid {

class SpectralGateModule : public GridModule {
  public:
    explicit SpectralGateModule(std::size_t window = 1024)
        : GridModule(4, 2), window_(window), hop_size_(window / 4), hann_(window) {
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
            ch.synth_cpx.resize(window_);
            ch.ifft_out.resize(window_);
            ch.acc.assign(2 * window_, 0.f);
            for (auto& cx : ch.in_cpx) {
                cx.r = 0.f;
                cx.i = 0.f;
            }
            ch.fwd_cfg = kiss_fft_alloc(N, 0 /* forward */, nullptr, nullptr);
            ch.inv_cfg = kiss_fft_alloc(N, 1 /* inverse */, nullptr, nullptr);
        }
    }

    ~SpectralGateModule() override {
        for (auto& ch : ch_) {
            if (ch.fwd_cfg)
                kiss_fft_free(ch.fwd_cfg);
            if (ch.inv_cfg)
                kiss_fft_free(ch.inv_cfg);
        }
    }

    SpectralGateModule(const SpectralGateModule&)            = delete;
    SpectralGateModule& operator=(const SpectralGateModule&) = delete;

    void prepare(const GridProcessArgs&) override {
        for (auto& out : outputs)
            out.voltage = 0.f;
        for (auto& ch : ch_) {
            std::fill(ch.ring.begin(), ch.ring.end(), 0.f);
            std::fill(ch.mag.begin(), ch.mag.end(), 0.f);
            std::fill(ch.acc.begin(), ch.acc.end(), 0.f);
            ch.wpos      = 0;
            ch.hop_count = 0;
            ch.acc_wpos  = 0;
            ch.acc_rpos  = 0;
        }
    }

    void process(const GridProcessArgs&) override {
        const float threshold = std::clamp(inputs[2].voltage, 0.f, 1.f);
        const float floor_val = std::clamp(inputs[3].voltage, 0.f, 1.f);

        for (int c = 0; c < 2; ++c) {
            auto& ch = ch_[c];

            ch.ring[ch.wpos] = inputs[c].voltage;
            ch.wpos          = (ch.wpos + 1) % window_;

            if (++ch.hop_count >= hop_size_) {
                ch.hop_count = 0;
                run_hop(ch, threshold, floor_val);
            }

            const std::size_t rpos = ch.acc_rpos % (2 * window_);
            outputs[c].voltage     = ch.acc[rpos];
            ch.acc[rpos]           = 0.f;
            ++ch.acc_rpos;
        }
    }

    std::size_t window() const noexcept { return window_; }
    std::size_t hop_size() const noexcept { return hop_size_; }

  private:
    static constexpr float kTwoPi = 6.28318530717958647692f;

    struct ChanState {
        std::vector<float>        ring;
        std::vector<kiss_fft_cpx> in_cpx;
        std::vector<kiss_fft_cpx> fwd_out;
        std::vector<float>        mag;
        std::vector<kiss_fft_cpx> synth_cpx;
        std::vector<kiss_fft_cpx> ifft_out;
        std::vector<float>        acc;
        kiss_fft_cfg              fwd_cfg{nullptr};
        kiss_fft_cfg              inv_cfg{nullptr};
        std::size_t               wpos{0};
        std::size_t               hop_count{0};
        std::size_t               acc_wpos{0};
        std::size_t               acc_rpos{0};
    };

    std::size_t              window_;
    std::size_t              hop_size_;
    std::vector<float>       hann_;
    std::array<ChanState, 2> ch_;

    void run_hop(ChanState& ch, float threshold, float floor_val) {
        // Forward FFT with Hann window.
        for (std::size_t i = 0; i < window_; ++i) {
            const std::size_t idx = (ch.wpos + i) % window_;
            ch.in_cpx[i].r        = ch.ring[idx] * hann_[i];
            ch.in_cpx[i].i        = 0.f;
        }
        kiss_fft(ch.fwd_cfg, ch.in_cpx.data(), ch.fwd_out.data());

        const std::size_t nb = window_ / 2 + 1;
        const std::size_t N  = window_;

        // Magnitudes and spectral peak.
        float max_mag = 0.f;
        for (std::size_t k = 0; k < nb; ++k) {
            const float r = ch.fwd_out[k].r, im = ch.fwd_out[k].i;
            ch.mag[k] = std::sqrt(r * r + im * im);
            if (ch.mag[k] > max_mag)
                max_mag = ch.mag[k];
        }

        const float gate_thr = max_mag * threshold;

        // Scale original complex values in-place — preserves phase relationships.
        for (std::size_t k = 0; k < nb; ++k) {
            const float s     = (ch.mag[k] >= gate_thr) ? 1.f : floor_val;
            ch.synth_cpx[k].r = ch.fwd_out[k].r * s;
            ch.synth_cpx[k].i = ch.fwd_out[k].i * s;
        }
        // DC and Nyquist must be real for real-valued IFFT output.
        ch.synth_cpx[0].i      = 0.f;
        ch.synth_cpx[nb - 1].i = 0.f;
        // Hermitian mirror: synth_cpx[N-k] = conj(synth_cpx[k]).
        for (std::size_t k = 1; k < nb - 1; ++k) {
            ch.synth_cpx[N - k].r = ch.synth_cpx[k].r;
            ch.synth_cpx[N - k].i = -ch.synth_cpx[k].i;
        }

        kiss_fft(ch.inv_cfg, ch.synth_cpx.data(), ch.ifft_out.data());

        const float inv_n = 1.f / static_cast<float>(N);
        for (std::size_t n = 0; n < N; ++n)
            ch.acc[(ch.acc_wpos + n) % (2 * N)] += ch.ifft_out[n].r * hann_[n] * inv_n;
        ch.acc_wpos = (ch.acc_wpos + hop_size_) % (2 * N);
    }
};

} // namespace kairos_grid
