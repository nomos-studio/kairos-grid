// SPDX-License-Identifier: GPL-3.0-or-later
// Low-pass gate — stmlib ZDF one-pole + VCA, optional vactrol simulation.
//
// Inputs:
//   0  in-l       — audio left
//   1  in-r       — audio right
//   2  cv         — vactrol intensity 0–1
//   3  decay      — vactrol release time: 0=50ms, 1=500ms (vactrol mode only)
//   4  character  — 0=pure VCA, 1=LP+VCA coupled
// Outputs:
//   0  out-l
//   1  out-r
//
// "lpg"         — cv maps instantaneously to cutoff + gain; no temporal dynamics
// "lpg-vactrol" — asymmetric one-pole on cv path (fast attack ~5ms, slow release
//                 50–500ms controlled by decay); nonlinear LDR-like cutoff curve;
//                 per-instance release spread for ensemble personality variation

#pragma once

#include <kairos_grid/grid_module.hpp>

#include <stmlib/dsp/filter.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace kairos_grid::mi {

class LpgModule : public GridModule {
  public:
    explicit LpgModule(bool vactrol_mode = false) : GridModule(5, 2), vactrol_mode_(vactrol_mode) {
        pole_l_.Init();
        pole_r_.Init();
        if (vactrol_mode_) {
            // Derive a per-instance release spread in [0.7, 1.3] from a
            // static counter.  Each LpgModule(true) gets its own personality —
            // the spread is deterministic but varies between instances so that
            // multiple vactrol LPGs in a patch feel like distinct vactrols.
            static std::atomic<uint32_t> s_count{0};
            uint32_t                     h = s_count.fetch_add(1, std::memory_order_relaxed);
            h ^= 0xdeadbeef;
            h ^= h << 13;
            h ^= h >> 17;
            h ^= h << 5;
            instance_spread_ = 0.7f + 0.6f * (static_cast<float>(h & 0xffff) / 65535.f);
        }
    }

    void prepare(const GridProcessArgs& args) override {
        // Precompute attack coefficient (constant across all samples).
        // Linear approximation alpha ≈ dt/tau is accurate to < 1e-5 when
        // tau >> dt (both conditions hold at audio rates with 5ms attack).
        if (vactrol_mode_)
            alpha_attack_ = args.sample_time / 0.005f;
    }

    void process(const GridProcessArgs& args) override {
        const float cv  = std::clamp(inputs[2].voltage, 0.f, 1.f);
        const float dec = std::clamp(inputs[3].voltage, 0.f, 1.f);
        const float chr = std::clamp(inputs[4].voltage, 0.f, 1.f);

        if (vactrol_mode_) {
            float alpha;
            if (cv > vactrol_) {
                alpha = alpha_attack_;
            } else {
                // Release tau = 50–500ms, scaled by per-instance spread.
                // Same linear approximation as attack — accurate when tau >> dt.
                const float tau_r = (0.050f + dec * 0.450f) * instance_spread_;
                alpha             = args.sample_time / tau_r;
            }
            vactrol_ += alpha * (cv - vactrol_);
            vactrol_ = std::clamp(vactrol_, 0.f, 1.f);
        } else {
            vactrol_ = cv;
        }

        // Filter cutoff: character=0 → f_hi constant (bypass LP effect, pure VCA);
        // character=1 → cutoff tracks vactrol² (LDR-like quadratic response).
        const float f_lo   = 20.f * args.sample_time;
        const float f_hi   = std::min(18000.f * args.sample_time, 0.495f);
        const float vc_sq  = vactrol_ * vactrol_;
        const float cutoff = f_lo + (f_hi - f_lo) * ((1.f - chr) + chr * vc_sq);

        pole_l_.set_f<stmlib::FREQUENCY_DIRTY>(cutoff);
        pole_r_.set_f<stmlib::FREQUENCY_DIRTY>(cutoff);

        // Gain is linear with vactrol state; filter handles timbre, VCA handles level.
        const float gain = vactrol_;
        outputs[0].voltage =
            pole_l_.Process<stmlib::FILTER_MODE_LOW_PASS>(inputs[0].voltage) * gain;
        outputs[1].voltage =
            pole_r_.Process<stmlib::FILTER_MODE_LOW_PASS>(inputs[1].voltage) * gain;
    }

  private:
    bool  vactrol_mode_;
    float vactrol_         = 0.f;
    float alpha_attack_    = 0.f;
    float instance_spread_ = 1.f;

    stmlib::OnePole pole_l_;
    stmlib::OnePole pole_r_;
};

} // namespace kairos_grid::mi
