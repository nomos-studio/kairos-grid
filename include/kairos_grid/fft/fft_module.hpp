// SPDX-License-Identifier: GPL-3.0-or-later
// fft — STFT analysis tap; spectral descriptors as CV.
//
// Observes an audio signal and emits three spectral descriptors per channel
// as CV outputs.  The module is a tap: connect the same source to both
// fft.in-l and downstream audio modules — fan-out is free, signal unaltered.
//
// Descriptors:
//   centroid  — brightness / register; [0,1] where 0=DC, 1=Nyquist
//   flatness  — noise-vs-tonal (Wiener entropy); 0=pure tone, 1=white noise
//   flux      — positive-only onset flux; 0=stable, 1=rapid change
//
// Port surface: 2 inputs, 6 outputs.
//   inputs[0]  in-l
//   inputs[1]  in-r
//   outputs[0] centroid-l   [0, 1]
//   outputs[1] centroid-r   [0, 1]
//   outputs[2] flatness-l   [0, 1]
//   outputs[3] flatness-r   [0, 1]
//   outputs[4] flux-l       [0, 1]
//   outputs[5] flux-r       [0, 1]
//
// Analysis method: per-channel ring buffer → Hann window → N-point complex
// FFT (using KissFFT with zero imaginary input) → magnitude for bins [0, N/2].
//
// Hop size: window / 4 (75% overlap).  Outputs hold the previous frame's
// values between hops.
//
// Latency (first valid output): window samples.
//   512  →  ~10.7ms at 48kHz
//   1024 →  ~21.3ms at 48kHz (default)
//   2048 →  ~42.7ms at 48kHz
//   4096 →  ~85.3ms at 48kHz
//
// Window size is patch-time (construction argument).  Registry types:
//   "fft"       — 1024-sample window (default)
//   "fft-512"   — 512-sample window
//   "fft-2048"  — 2048-sample window
//   "fft-4096"  — 4096-sample window
//
// Window must be a power of two for best KissFFT performance (it works with
// any N but powers of two are the fast path).
//
// :buf-id protocol (Phase 2):
//   The L and R magnitude arrays are stored as shared_ptr<vector<float>> so the
//   factory can inject pre-created storage.  A peek module on the same :buf-id
//   reads directly from the live magnitude buffer; index [0, 1] maps to bins
//   [0, window/2] (DC to Nyquist).
//
//   Standalone construction (no :buf-id): FftModule owns its own mag storage.
//   Shared construction (factory with :buf-id): factory provides pre-created
//   shared_ptrs; mag_l_ptr() / mag_r_ptr() hand them to downstream peek modules.

#pragma once

#include <kairos_grid/grid_module.hpp>
#include <kiss_fft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace kairos_grid {

class FftModule : public GridModule {
  public:
    // Standalone: allocates its own magnitude storage.
    explicit FftModule(std::size_t window = 1024)
        : FftModule(window, std::make_shared<std::vector<float>>(window / 2 + 1, 0.f),
                    std::make_shared<std::vector<float>>(window / 2 + 1, 0.f)) {}

    // Shared (:buf-id): factory provides pre-created magnitude buffers.
    // The shared_ptrs are written by this module and readable by peer peek modules.
    FftModule(std::size_t window, std::shared_ptr<std::vector<float>> mag_l,
              std::shared_ptr<std::vector<float>> mag_r)
        : GridModule(2, 6), window_(window), hop_size_(window / 4), hann_(window) {
        const float denom = static_cast<float>(window_ > 1u ? window_ - 1u : 1u);
        for (std::size_t i = 0; i < window_; ++i)
            hann_[i] =
                0.5f *
                (1.f - std::cos(2.f * 3.14159265358979323846f * static_cast<float>(i) / denom));

        const int N = static_cast<int>(window_);
        init_channel(ch_[0], N, std::move(mag_l));
        init_channel(ch_[1], N, std::move(mag_r));
    }

    ~FftModule() override {
        for (auto& ch : ch_)
            if (ch.cfg)
                kiss_fft_free(ch.cfg);
    }

    FftModule(const FftModule&)            = delete;
    FftModule& operator=(const FftModule&) = delete;

    void prepare(const GridProcessArgs&) override {
        for (auto& out : outputs)
            out.voltage = 0.f;
        for (auto& ch : ch_) {
            std::fill(ch.ring.begin(), ch.ring.end(), 0.f);
            std::fill(ch.mag->begin(), ch.mag->end(), 0.f);
            std::fill(ch.prev_mag.begin(), ch.prev_mag.end(), 0.f);
            ch.wpos      = 0;
            ch.hop_count = 0;
            ch.centroid  = 0.f;
            ch.flatness  = 0.f;
            ch.flux      = 0.f;
        }
    }

    void process(const GridProcessArgs&) override {
        update_channel(ch_[0], inputs[0].voltage);
        update_channel(ch_[1], inputs[1].voltage);
        outputs[0].voltage = ch_[0].centroid;
        outputs[1].voltage = ch_[1].centroid;
        outputs[2].voltage = ch_[0].flatness;
        outputs[3].voltage = ch_[1].flatness;
        outputs[4].voltage = ch_[0].flux;
        outputs[5].voltage = ch_[1].flux;
    }

    // Magnitude array accessors — length is window/2+1 (DC to Nyquist).
    const std::vector<float>& mag_l() const noexcept { return *ch_[0].mag; }
    const std::vector<float>& mag_r() const noexcept { return *ch_[1].mag; }

    // Shared-pointer accessors — hand to PeekModule for :buf-id wiring.
    std::shared_ptr<std::vector<float>> mag_l_ptr() const noexcept { return ch_[0].mag; }
    std::shared_ptr<std::vector<float>> mag_r_ptr() const noexcept { return ch_[1].mag; }

    std::size_t window() const noexcept { return window_; }
    std::size_t hop_size() const noexcept { return hop_size_; }

  private:
    struct ChanState {
        std::vector<float>                  ring;
        std::vector<float>                  frame;
        std::vector<kiss_fft_cpx>           in_cpx;
        std::vector<kiss_fft_cpx>           out_cpx;
        std::shared_ptr<std::vector<float>> mag;      // shared; written here, readable by peek
        std::vector<float>                  prev_mag; // internal; flux computation only
        kiss_fft_cfg                        cfg{nullptr};
        std::size_t                         wpos{0};
        std::size_t                         hop_count{0};
        float                               centroid{0.f};
        float                               flatness{0.f};
        float                               flux{0.f};
    };

    std::size_t              window_;
    std::size_t              hop_size_;
    std::vector<float>       hann_;
    std::array<ChanState, 2> ch_;

    static void init_channel(ChanState& ch, int N, std::shared_ptr<std::vector<float>> mag_ptr) {
        ch.ring.assign(static_cast<std::size_t>(N), 0.f);
        ch.frame.assign(static_cast<std::size_t>(N), 0.f);
        ch.in_cpx.resize(static_cast<std::size_t>(N));
        ch.out_cpx.resize(static_cast<std::size_t>(N));
        ch.mag = std::move(mag_ptr);
        ch.prev_mag.assign(static_cast<std::size_t>(N) / 2 + 1, 0.f);
        for (auto& c : ch.in_cpx) {
            c.r = 0.f;
            c.i = 0.f;
        }
        ch.cfg = kiss_fft_alloc(N, 0 /* forward */, nullptr, nullptr);
    }

    void update_channel(ChanState& ch, float sample) {
        ch.ring[ch.wpos] = sample;
        ch.wpos          = (ch.wpos + 1u) % window_;
        ++ch.hop_count;

        if (ch.hop_count < hop_size_)
            return;
        ch.hop_count = 0;

        // Unwrap ring buffer into frame and apply Hann window.
        for (std::size_t i = 0; i < window_; ++i) {
            const std::size_t idx = (ch.wpos + i) % window_;
            ch.frame[i]           = ch.ring[idx] * hann_[i];
        }

        for (std::size_t i = 0; i < window_; ++i) {
            ch.in_cpx[i].r = ch.frame[i];
            ch.in_cpx[i].i = 0.f;
        }
        kiss_fft(ch.cfg, ch.in_cpx.data(), ch.out_cpx.data());

        const std::size_t nb  = window_ / 2 + 1;
        auto&             mag = *ch.mag;

        for (std::size_t k = 0; k < nb; ++k) {
            const float r  = ch.out_cpx[k].r;
            const float im = ch.out_cpx[k].i;
            mag[k]         = std::sqrt(r * r + im * im);
        }

        // Spectral centroid [0, 1].
        float sum_km = 0.f, sum_m = 0.f;
        for (std::size_t k = 0; k < nb; ++k) {
            sum_km += static_cast<float>(k) * mag[k];
            sum_m += mag[k];
        }
        ch.centroid = (sum_m > 1e-8f)
                          ? std::clamp(sum_km / sum_m / static_cast<float>(nb - 1u), 0.f, 1.f)
                          : 0.f;

        // Spectral flatness [0, 1] — Wiener entropy.
        constexpr float eps     = 1e-10f;
        float           sum_log = 0.f, sum_a = 0.f;
        for (std::size_t k = 0; k < nb; ++k) {
            const float m = mag[k] + eps;
            sum_log += std::log(m);
            sum_a += m;
        }
        const float geo = std::exp(sum_log / static_cast<float>(nb));
        const float ari = sum_a / static_cast<float>(nb);
        ch.flatness     = std::clamp(geo / ari, 0.f, 1.f);

        // Positive-only spectral flux [0, 1] — onset energy.
        float sum_flux = 0.f, sum_nrm = 0.f;
        for (std::size_t k = 0; k < nb; ++k) {
            const float diff = mag[k] - ch.prev_mag[k];
            sum_flux += (diff > 0.f) ? diff : 0.f;
            sum_nrm += mag[k] + ch.prev_mag[k] + eps;
        }
        ch.flux = std::clamp(2.f * sum_flux / sum_nrm, 0.f, 1.f);

        std::copy(mag.begin(), mag.end(), ch.prev_mag.begin());
    }
};

} // namespace kairos_grid
