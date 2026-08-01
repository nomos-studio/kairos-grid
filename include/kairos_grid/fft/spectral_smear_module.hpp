// SPDX-License-Identifier: GPL-3.0-or-later
// spectral-smear — Panharmonium-style temporal spectral averaging + IFFT resynthesis.
//
// Always running: continuously analyzes audio via STFT, maintains a per-channel
// exponential moving average of the magnitude spectrum (the "smear"), then
// resynthesizes the smoothed spectrum via IFFT with randomized per-hop phases.
// A density threshold retains only the strongest spectral components.
//
// Port surface: 4 inputs, 2 outputs.
//   inputs[0]  in-l     audio input, left channel
//   inputs[1]  in-r     audio input, right channel
//   inputs[2]  smear    [0, 1] — 0 = live resynthesis, 1 = spectrum freezes
//   inputs[3]  density  [0, 1] — 1 = all bins, 0 = loudest bin only
//   outputs[0] out-l    resynthesized audio, left channel
//   outputs[1] out-r    resynthesized audio, right channel
//
// Smear parameter (sampled per hop):
//   smoothed_mag[k] = α * smoothed_mag[k] + (1 − α) * current_mag[k]
//   where α = clamp(smear, 0, 1).
//   At α = 0: smoothed_mag tracks each frame (live spectrally-shaped noise).
//   At α → 1: smoothed_mag decays very slowly (Panharmonium freeze character).
//
// Density threshold (applied per hop after smoothing):
//   threshold = max(smoothed_mag) * (1 − density)
//   Bins below threshold are zeroed before synthesis.
//
// Phase model: random phases per hop — same Panharmonium wash character as
// spectral-freeze.  Output starts silent and builds over the first window/hop_size
// hops as smoothed_mag populates from zero.
//
// Registry types (all behind KAIROS_GRID_BUILD_FFT):
//   "spectral-smear"       — 1024-sample window (default, ~21 ms at 48 kHz)
//   "spectral-smear-512"   — 512-sample window
//   "spectral-smear-2048"  — 2048-sample window
//   "spectral-smear-4096"  — 4096-sample window

#pragma once

#include <kairos_grid/grid_module.hpp>
#include <kiss_fft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace kairos_grid {

class SpectralSmearModule : public GridModule {
  public:
    explicit SpectralSmearModule(std::size_t window = 1024)
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
            ch.smoothed_mag.assign(window_ / 2 + 1, 0.f);
            ch.synth_mag.assign(window_ / 2 + 1, 0.f);
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

    ~SpectralSmearModule() override {
        for (auto& ch : ch_) {
            if (ch.fwd_cfg)
                kiss_fft_free(ch.fwd_cfg);
            if (ch.inv_cfg)
                kiss_fft_free(ch.inv_cfg);
        }
    }

    SpectralSmearModule(const SpectralSmearModule&)            = delete;
    SpectralSmearModule& operator=(const SpectralSmearModule&) = delete;

    void prepare(const GridProcessArgs&) override {
        for (auto& out : outputs)
            out.voltage = 0.f;
        for (auto& ch : ch_) {
            std::fill(ch.ring.begin(), ch.ring.end(), 0.f);
            std::fill(ch.mag.begin(), ch.mag.end(), 0.f);
            std::fill(ch.smoothed_mag.begin(), ch.smoothed_mag.end(), 0.f);
            std::fill(ch.synth_mag.begin(), ch.synth_mag.end(), 0.f);
            std::fill(ch.acc.begin(), ch.acc.end(), 0.f);
            ch.wpos        = 0;
            ch.hop_count   = 0;
            ch.acc_wpos    = 0;
            ch.acc_rpos    = 0;
            ch.initialized = false;
        }
    }

    void process(const GridProcessArgs&) override {
        // Sample CVs once per process() call; spectral processing uses them at hop time.
        const float smear   = std::clamp(inputs[2].voltage, 0.f, 1.f);
        const float density = std::clamp(inputs[3].voltage, 0.f, 1.f);

        for (int c = 0; c < 2; ++c) {
            auto& ch = ch_[c];

            ch.ring[ch.wpos] = inputs[c].voltage;
            ch.wpos          = (ch.wpos + 1) % window_;

            if (++ch.hop_count >= hop_size_) {
                ch.hop_count = 0;
                run_analysis_hop(ch, smear, density);
                do_synthesis_hop(ch);
            }

            const std::size_t rpos = ch.acc_rpos % (2 * window_);
            outputs[c].voltage     = ch.acc[rpos];
            ch.acc[rpos]           = 0.f;
            ++ch.acc_rpos;
        }
    }

    std::size_t window() const noexcept { return window_; }
    std::size_t hop_size() const noexcept { return hop_size_; }

    // Read access for tests and future :buf-id integration.
    const std::vector<float>& smoothed_mag_l() const noexcept { return ch_[0].smoothed_mag; }
    const std::vector<float>& smoothed_mag_r() const noexcept { return ch_[1].smoothed_mag; }

  private:
    static constexpr float kTwoPi = 6.28318530717958647692f;

    struct ChanState {
        // Analysis
        std::vector<float>        ring;
        std::vector<kiss_fft_cpx> in_cpx;
        std::vector<kiss_fft_cpx> fwd_out;
        std::vector<float>        mag;
        std::vector<float>        smoothed_mag; // exponential moving average
        std::vector<float>        synth_mag;    // post-density-threshold
        kiss_fft_cfg              fwd_cfg{nullptr};
        std::size_t               wpos{0};
        std::size_t               hop_count{0};
        bool                      initialized{false}; // first hop seeds smoothed_mag directly

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

    float next_phase(ChanState& ch) noexcept {
        ch.rng          = ch.rng * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto bits = static_cast<uint32_t>(ch.rng >> 33);
        return static_cast<float>(bits) * (kTwoPi / static_cast<float>(0x80000000u));
    }

    void run_analysis_hop(ChanState& ch, float smear, float density) {
        // Forward FFT with Hann window.
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

        // First hop seeds smoothed_mag directly so smear=1 doesn't start frozen at zero.
        // Subsequent hops apply the exponential moving average.
        if (!ch.initialized) {
            ch.smoothed_mag = ch.mag;
            ch.initialized  = true;
        } else {
            const float alpha = smear;
            const float beta  = 1.f - alpha;
            for (std::size_t k = 0; k < nb; ++k)
                ch.smoothed_mag[k] = alpha * ch.smoothed_mag[k] + beta * ch.mag[k];
        }

        // Density threshold: retain bins above max_mag * (1 − density).
        const float max_m     = *std::max_element(ch.smoothed_mag.begin(), ch.smoothed_mag.end());
        const float threshold = max_m * (1.f - density);
        for (std::size_t k = 0; k < nb; ++k)
            ch.synth_mag[k] = (ch.smoothed_mag[k] >= threshold) ? ch.smoothed_mag[k] : 0.f;
    }

    void do_synthesis_hop(ChanState& ch) {
        const std::size_t nb = window_ / 2 + 1;
        const std::size_t N  = window_;

        // Hermitian spectrum from synth_mag + random phases.
        ch.synth_cpx[0]      = {ch.synth_mag[0], 0.f};
        ch.synth_cpx[nb - 1] = {ch.synth_mag[nb - 1], 0.f};

        for (std::size_t k = 1; k < nb - 1; ++k) {
            const float phi     = next_phase(ch);
            const float cs      = ch.synth_mag[k] * std::cos(phi);
            const float sn      = ch.synth_mag[k] * std::sin(phi);
            ch.synth_cpx[k]     = {cs, sn};
            ch.synth_cpx[N - k] = {cs, -sn};
        }

        kiss_fft(ch.inv_cfg, ch.synth_cpx.data(), ch.ifft_out.data());

        const float scale = 1.f / static_cast<float>(N);
        for (std::size_t n = 0; n < N; ++n) {
            ch.acc[(ch.acc_wpos + n) % (2 * N)] += ch.ifft_out[n].r * hann_[n] * scale;
        }
        ch.acc_wpos = (ch.acc_wpos + hop_size_) % (2 * N);
    }
};

} // namespace kairos_grid
