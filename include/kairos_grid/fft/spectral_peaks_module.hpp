// SPDX-License-Identifier: GPL-3.0-or-later
// spectral-peaks — STFT peak detector; up to 8 spectral peaks as CV.
//
// Analyzes a mono mix of the stereo input via STFT and detects the N strongest
// local maxima in the magnitude spectrum each hop.  Sub-bin frequency accuracy
// via parabolic interpolation.  Outputs are amplitude-sorted (strongest peak in
// slot 0) and held between hops.  A one-sample trigger pulse fires on each
// analysis hop; wire it to a partial-tracker to synchronize tracking updates.
//
// Port surface: 3 inputs, 17 outputs.
//   inputs[0]   in-l       audio input, left channel
//   inputs[1]   in-r       audio input, right channel
//   inputs[2]   threshold  [0, 1] — minimum relative magnitude for peak
//                            detection; 0 = any local max, 1 = only global peak
//
//   outputs[0..7]   freq_0..freq_7   normalized frequency [0, 1] (DC→Nyquist);
//                                    0 if slot unused
//   outputs[8..15]  amp_0..amp_7    normalized amplitude [0, 1] relative to
//                                    spectral peak; 0 if slot unused
//   outputs[16]     trigger          1.0 for one sample on each analysis hop
//
// N = 8 peaks maximum; slots filled in descending amplitude order.
//
// Analysis: STFT of (in-l + in-r) / 2 with Hann window.
// Hop: window / 4 (75% overlap).  Latency: window samples.
//
// Peak detection: interior bins only (k = 1 … window/2 − 1).
// DC (k=0) and Nyquist (k=window/2) are excluded — they cannot satisfy the
// both-neighbor local-maximum test for real spectral content.
//
// Parabolic interpolation:
//   k_frac = k + 0.5 * (mag[k−1] − mag[k+1]) / (mag[k−1] − 2*mag[k] + mag[k+1])
//   freq = clamp(k_frac / (window/2), 0, 1)
//
// Registry types (all behind KAIROS_GRID_BUILD_FFT):
//   "spectral-peaks"       — 1024-sample window
//   "spectral-peaks-512"   — 512-sample window
//   "spectral-peaks-2048"  — 2048-sample window
//   "spectral-peaks-4096"  — 4096-sample window

#pragma once

#include <kairos_grid/grid_module.hpp>
#include <kiss_fft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace kairos_grid {

class SpectralPeaksModule : public GridModule {
  public:
    static constexpr std::size_t kMaxPeaks = 8;

    explicit SpectralPeaksModule(std::size_t window = 1024)
        : GridModule(3, 2 * kMaxPeaks + 1), window_(window), hop_size_(window / 4), hann_(window),
          ring_(window, 0.f), in_cpx_(window), fwd_out_(window), mag_(window / 2 + 1, 0.f) {
        const float denom = static_cast<float>(window_ > 1u ? window_ - 1u : 1u);
        for (std::size_t i = 0; i < window_; ++i)
            hann_[i] = 0.5f * (1.f - std::cos(kTwoPi * static_cast<float>(i) / denom));
        for (auto& cx : in_cpx_) {
            cx.r = 0.f;
            cx.i = 0.f;
        }
        candidates_.reserve(window / 2);
        fwd_cfg_ = kiss_fft_alloc(static_cast<int>(window_), 0, nullptr, nullptr);

        // Performance taps: per-slot peak freq + amp, exposed to the tap bus so the
        // analysis is readable over IPC (the spectral "ears"). taps[0..7] = freq,
        // taps[8..15] = amp, index-aligned with outputs[0..15]. The hop trigger
        // (outputs[16]) is not tapped — a one-sample pulse is meaningless at the
        // ~30 Hz tap telemetry rate.
        taps.reserve(2 * kMaxPeaks);
        for (std::size_t i = 0; i < kMaxPeaks; ++i)
            taps.push_back({"spectral/peak-" + std::to_string(i) + "-freq", 0.f});
        for (std::size_t i = 0; i < kMaxPeaks; ++i)
            taps.push_back({"spectral/peak-" + std::to_string(i) + "-amp", 0.f});
    }

    ~SpectralPeaksModule() override {
        if (fwd_cfg_)
            kiss_fft_free(fwd_cfg_);
    }

    SpectralPeaksModule(const SpectralPeaksModule&)            = delete;
    SpectralPeaksModule& operator=(const SpectralPeaksModule&) = delete;

    void prepare(const GridProcessArgs&) override {
        for (auto& out : outputs)
            out.voltage = 0.f;
        std::fill(ring_.begin(), ring_.end(), 0.f);
        std::fill(mag_.begin(), mag_.end(), 0.f);
        n_peaks_   = 0;
        wpos_      = 0;
        hop_count_ = 0;
        for (auto& p : peaks_) {
            p.freq = 0.f;
            p.amp  = 0.f;
        }
    }

    void process(const GridProcessArgs&) override {
        const float sample    = (inputs[0].voltage + inputs[1].voltage) * 0.5f;
        const float threshold = std::clamp(inputs[2].voltage, 0.f, 1.f);

        ring_[wpos_] = sample;
        wpos_        = (wpos_ + 1) % window_;

        bool hop_fired = false;
        if (++hop_count_ >= hop_size_) {
            hop_count_ = 0;
            run_hop(threshold);
            hop_fired = true;
        }

        for (std::size_t i = 0; i < kMaxPeaks; ++i) {
            const float f                  = (i < n_peaks_) ? peaks_[i].freq : 0.f;
            const float a                  = (i < n_peaks_) ? peaks_[i].amp : 0.f;
            outputs[i].voltage             = f;
            outputs[kMaxPeaks + i].voltage = a;
            taps[i].value                  = f; // spectral/peak-i-freq
            taps[kMaxPeaks + i].value      = a; // spectral/peak-i-amp
        }
        outputs[2 * kMaxPeaks].voltage = hop_fired ? 1.f : 0.f;
    }

    std::size_t window() const noexcept { return window_; }
    std::size_t hop_size() const noexcept { return hop_size_; }
    std::size_t n_peaks() const noexcept { return n_peaks_; }

  private:
    static constexpr float kTwoPi = 6.28318530717958647692f;

    struct Peak {
        float freq{0.f};
        float amp{0.f};
    };

    std::size_t               window_;
    std::size_t               hop_size_;
    std::vector<float>        hann_;
    std::vector<float>        ring_;
    std::vector<kiss_fft_cpx> in_cpx_;
    std::vector<kiss_fft_cpx> fwd_out_;
    std::vector<float>        mag_;
    kiss_fft_cfg              fwd_cfg_{nullptr};
    std::size_t               wpos_{0};
    std::size_t               hop_count_{0};

    std::array<Peak, kMaxPeaks> peaks_{};
    std::size_t                 n_peaks_{0};
    std::vector<Peak>           candidates_; // pre-allocated scratch

    void run_hop(float threshold) {
        // Forward FFT with Hann window on mono mix.
        for (std::size_t i = 0; i < window_; ++i) {
            const std::size_t idx = (wpos_ + i) % window_;
            in_cpx_[i].r          = ring_[idx] * hann_[i];
            in_cpx_[i].i          = 0.f;
        }
        kiss_fft(fwd_cfg_, in_cpx_.data(), fwd_out_.data());

        const std::size_t nb = window_ / 2 + 1;

        float max_mag = 0.f;
        for (std::size_t k = 0; k < nb; ++k) {
            const float r = fwd_out_[k].r, im = fwd_out_[k].i;
            mag_[k] = std::sqrt(r * r + im * im);
            if (mag_[k] > max_mag)
                max_mag = mag_[k];
        }

        const float thr = max_mag * threshold;

        candidates_.clear();
        for (std::size_t k = 1; k + 1 < nb; ++k) {
            if (mag_[k] > mag_[k - 1] && mag_[k] > mag_[k + 1] && mag_[k] >= thr) {
                const float a = mag_[k - 1], b = mag_[k], c = mag_[k + 1];
                const float denom = a - 2.f * b + c;
                float       kfp   = static_cast<float>(k);
                if (std::abs(denom) > 1e-10f)
                    kfp += 0.5f * (a - c) / denom;
                const float fn = std::clamp(kfp / static_cast<float>(nb - 1u), 0.f, 1.f);
                const float an = (max_mag > 0.f) ? mag_[k] / max_mag : 0.f;
                candidates_.push_back({fn, an});
            }
        }

        // Partial-sort: strongest kMaxPeaks at the front.
        const std::size_t n_sort = std::min(candidates_.size(), kMaxPeaks);
        std::partial_sort(
            candidates_.begin(), candidates_.begin() + static_cast<std::ptrdiff_t>(n_sort),
            candidates_.end(), [](const Peak& a, const Peak& b) { return a.amp > b.amp; });

        n_peaks_ = n_sort;
        for (std::size_t i = 0; i < n_peaks_; ++i)
            peaks_[i] = candidates_[i];
    }
};

} // namespace kairos_grid
