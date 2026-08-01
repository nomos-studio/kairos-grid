// SPDX-License-Identifier: GPL-3.0-or-later
// bin-shift — frequency-domain pitch shift by integer bin displacement.
//
// Analyzes audio via STFT, shifts every bin by a fixed number of FFT bins
// (upward or downward), then resynthesizes using the original FFT phases.
// Bins shifted past DC or Nyquist are zeroed.  Artifact-free for gentle
// amounts; inharmonic ring-modulator character at large amounts.
//
// This is a uniform-Hz frequency transposition (all partials shift by the
// same Hz amount), not a ratio-preserving pitch shift.  The distinction is
// musically significant: gentle bin shifts produce subtle detuning and
// thickening; large shifts produce inharmonic, additive-synthesis-like timbres.
//
// Port surface: 3 inputs, 2 outputs.
//   inputs[0]  in-l    audio input, left channel
//   inputs[1]  in-r    audio input, right channel
//   inputs[2]  shift   [-1, 1] — bin displacement;
//                       0 = no shift (STFT round-trip)
//                      +1 = maximum upshift (all content at/past Nyquist)
//                      -1 = maximum downshift (all content at/below DC)
//                       Internally: s = round(shift * (window/2))
//   outputs[0] out-l   shifted audio, left channel
//   outputs[1] out-r   shifted audio, right channel
//
// Shift semantics:
//   s = round(clamp(shift, -1, 1) * (nb - 1))  where nb = window/2 + 1
//   synth_cpx[k] = fwd_out[k - s]   if k - s ∈ [0, nb)
//                = {0, 0}            otherwise
//   Original FFT phases preserved — result is spectrally translated audio.
//
// Overlap-add: Hann window pre-FFT and post-IFFT; hop = window/4 (75% overlap).
// IFFT normalized by 1/N; OLA accumulation buffer is 2*window samples circular.
//
// Registry types (all behind KAIROS_GRID_BUILD_FFT):
//   "bin-shift"       — 1024-sample window (default, ~21 ms at 48 kHz)
//   "bin-shift-512"   — 512-sample window
//   "bin-shift-2048"  — 2048-sample window
//   "bin-shift-4096"  — 4096-sample window

#pragma once

#include <kairos_grid/grid_module.hpp>
#include <kiss_fft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace kairos_grid {

class BinShiftModule : public GridModule {
  public:
    explicit BinShiftModule(std::size_t window = 1024)
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

    ~BinShiftModule() override {
        for (auto& ch : ch_) {
            if (ch.fwd_cfg)
                kiss_fft_free(ch.fwd_cfg);
            if (ch.inv_cfg)
                kiss_fft_free(ch.inv_cfg);
        }
    }

    BinShiftModule(const BinShiftModule&)            = delete;
    BinShiftModule& operator=(const BinShiftModule&) = delete;

    void prepare(const GridProcessArgs&) override {
        for (auto& out : outputs)
            out.voltage = 0.f;
        for (auto& ch : ch_) {
            std::fill(ch.ring.begin(), ch.ring.end(), 0.f);
            std::fill(ch.acc.begin(), ch.acc.end(), 0.f);
            ch.wpos      = 0;
            ch.hop_count = 0;
            ch.acc_wpos  = 0;
            ch.acc_rpos  = 0;
        }
    }

    void process(const GridProcessArgs&) override {
        const float shift_cv = std::clamp(inputs[2].voltage, -1.f, 1.f);

        for (int c = 0; c < 2; ++c) {
            auto& ch = ch_[c];

            ch.ring[ch.wpos] = inputs[c].voltage;
            ch.wpos          = (ch.wpos + 1) % window_;

            if (++ch.hop_count >= hop_size_) {
                ch.hop_count = 0;
                run_hop(ch, shift_cv);
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

    void run_hop(ChanState& ch, float shift_cv) {
        // Forward FFT with Hann window.
        for (std::size_t i = 0; i < window_; ++i) {
            const std::size_t idx = (ch.wpos + i) % window_;
            ch.in_cpx[i].r        = ch.ring[idx] * hann_[i];
            ch.in_cpx[i].i        = 0.f;
        }
        kiss_fft(ch.fwd_cfg, ch.in_cpx.data(), ch.fwd_out.data());

        const std::size_t nb = window_ / 2 + 1;
        const std::size_t N  = window_;

        // Integer bin shift: positive = upshift (higher frequencies).
        const int s =
            static_cast<int>(std::round(shift_cv * static_cast<float>(static_cast<int>(nb) - 1)));

        // Build shifted spectrum from original complex values (phase preserved).
        for (std::size_t k = 0; k < nb; ++k) {
            const int src = static_cast<int>(k) - s;
            if (src >= 0 && src < static_cast<int>(nb)) {
                ch.synth_cpx[k].r = ch.fwd_out[static_cast<std::size_t>(src)].r;
                ch.synth_cpx[k].i = ch.fwd_out[static_cast<std::size_t>(src)].i;
            } else {
                ch.synth_cpx[k] = {0.f, 0.f};
            }
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
