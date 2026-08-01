// SPDX-License-Identifier: GPL-3.0-or-later
// partial-tracker — polyphonic voice allocator for spectral peaks.
//
// Receives up to 8 spectral peaks (frequency + amplitude) from a
// spectral-peaks module via CV cables, and maintains M=8 persistent voice
// slots that track partials across analysis hops.  Voices glide smoothly
// between hop updates via per-sample exponential interpolation; the same
// smoothing coefficient governs both attack (tracking) and release (decay).
//
// Intended usage: spectral-peaks → partial-tracker → oscillator bank.
//
// Port surface: 18 inputs, 16 outputs.
//   inputs[0..7]    freq_0..freq_7    peak frequencies from spectral-peaks [0, 1]
//   inputs[8..15]   amp_0..amp_7     peak amplitudes from spectral-peaks [0, 1]
//   inputs[16]      trigger           rising-edge trigger from spectral-peaks
//   inputs[17]      smooth            [0, 1] — per-sample interpolation coefficient;
//                                      0 = immediate snap, 1 = no movement
//
//   outputs[0..7]   voice_freq_0..7   tracked frequencies, [0, 1]
//   outputs[8..15]  voice_amp_0..7    tracked amplitudes, [0, 1]; decays to 0
//                                      when the corresponding partial disappears
//
// Voice allocation (on trigger rising edge):
//   1. Each active voice greedily matches the nearest unmatched input peak
//      within kFreqMatchThreshold = 0.1 (normalized).  Unmatched voices go
//      inactive (amplitude decays to 0 at the smoothing rate).
//   2. Unmatched input peaks are assigned to the lowest-indexed voice slot
//      whose amplitude is below kDeathThreshold = 0.001.  On birth, the
//      voice frequency snaps to the peak frequency to prevent glide from a
//      stale position.
//
// Per-sample update (applied every sample, not only on trigger):
//   beta = 1 - smooth
//   active voice:   smooth_freq += beta * (target_freq - smooth_freq)
//                   smooth_amp  += beta * (target_amp  - smooth_amp)
//   inactive voice: smooth_amp  += beta * (0 - smooth_amp)   // release
//                   smooth_freq unchanged (held at last tracked value)
//
// Slots are not frequency-sorted — the same voice slot retains ownership of
// a partial for the duration of its life, enabling stable oscillator-bank
// assignment.  Voice j always drives oscillator j, regardless of pitch order.

#pragma once

#include <kairos_grid/grid_module.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace kairos_grid {

class PartialTrackerModule : public GridModule {
  public:
    static constexpr std::size_t kMaxPeaks    = 8;
    static constexpr std::size_t kMaxPartials = 8;

    // Frequency distance threshold for peak-to-voice matching (normalized [0,1]).
    static constexpr float kFreqMatchThreshold = 0.1f;
    // Voice slot is available for reuse when smooth_amp falls below this.
    static constexpr float kDeathThreshold = 0.001f;

    PartialTrackerModule() : GridModule(2 * kMaxPeaks + 2, 2 * kMaxPartials) {}

    ~PartialTrackerModule() override = default;

    PartialTrackerModule(const PartialTrackerModule&)            = delete;
    PartialTrackerModule& operator=(const PartialTrackerModule&) = delete;

    void prepare(const GridProcessArgs&) override {
        for (auto& out : outputs)
            out.voltage = 0.f;
        for (auto& v : voices_)
            v = Voice{};
        prev_trig_ = false;
    }

    void process(const GridProcessArgs&) override {
        // Rising-edge trigger detection.
        const bool trig_high = inputs[2 * kMaxPeaks].voltage > 0.5f;
        if (trig_high && !prev_trig_)
            do_matching();
        prev_trig_ = trig_high;

        // Per-sample exponential interpolation.
        const float alpha = std::clamp(inputs[2 * kMaxPeaks + 1].voltage, 0.f, 1.f);
        const float beta  = 1.f - alpha;

        for (std::size_t j = 0; j < kMaxPartials; ++j) {
            auto& v = voices_[j];
            if (v.active) {
                v.smooth_freq += beta * (v.target_freq - v.smooth_freq);
                v.smooth_amp += beta * (v.target_amp - v.smooth_amp);
            } else {
                v.smooth_amp += beta * (0.f - v.smooth_amp);
                // smooth_freq held at last tracked value
            }
            outputs[j].voltage                = v.smooth_freq;
            outputs[kMaxPartials + j].voltage = v.smooth_amp;
        }
    }

  private:
    struct Voice {
        float target_freq{0.f};
        float target_amp{0.f};
        float smooth_freq{0.f};
        float smooth_amp{0.f};
        bool  active{false};
    };

    std::array<Voice, kMaxPartials> voices_{};
    bool                            prev_trig_{false};

    void do_matching() {
        // Snapshot current peak inputs.
        std::array<float, kMaxPeaks> pfreq{}, pamp{};
        for (std::size_t i = 0; i < kMaxPeaks; ++i) {
            pfreq[i] = inputs[i].voltage;
            pamp[i]  = inputs[kMaxPeaks + i].voltage;
        }

        // Match each active voice to its nearest unmatched input peak.
        bool peak_used[kMaxPeaks] = {};
        // Track which voices were killed in this pass so step 2 can reuse
        // them immediately (their smooth_amp hasn't decayed yet).
        bool just_killed[kMaxPartials] = {};

        for (std::size_t j = 0; j < kMaxPartials; ++j) {
            auto& v = voices_[j];
            if (!v.active)
                continue;
            int   best_i    = -1;
            float best_dist = kFreqMatchThreshold;
            for (std::size_t i = 0; i < kMaxPeaks; ++i) {
                if (peak_used[i] || pamp[i] <= 0.f)
                    continue;
                const float dist = std::abs(pfreq[i] - v.target_freq);
                if (dist < best_dist) {
                    best_dist = dist;
                    best_i    = static_cast<int>(i);
                }
            }
            if (best_i >= 0) {
                v.target_freq     = pfreq[best_i];
                v.target_amp      = pamp[best_i];
                peak_used[best_i] = true;
            } else {
                v.active       = false; // no match within threshold — partial ended
                just_killed[j] = true;
            }
        }

        // Assign unmatched peaks to available slots.
        // A slot is available if it is inactive AND either just killed in this
        // pass (reuse immediately) or its amplitude has decayed to near zero.
        for (std::size_t i = 0; i < kMaxPeaks; ++i) {
            if (peak_used[i] || pamp[i] <= 0.f)
                continue;
            for (std::size_t j = 0; j < kMaxPartials; ++j) {
                auto& v = voices_[j];
                if (!v.active && (just_killed[j] || v.smooth_amp < kDeathThreshold)) {
                    v.target_freq  = pfreq[i];
                    v.target_amp   = pamp[i];
                    v.smooth_freq  = pfreq[i]; // snap to avoid glide from stale position
                    v.smooth_amp   = 0.f;
                    v.active       = true;
                    just_killed[j] = false; // slot consumed
                    break;
                }
            }
            // Peak discarded if no available slot.
        }
    }
};

} // namespace kairos_grid
