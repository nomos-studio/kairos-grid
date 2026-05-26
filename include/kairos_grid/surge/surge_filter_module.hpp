// SPDX-License-Identifier: GPL-3.0-or-later
// Surge XT sst-filters wrappers as GridModules.

#pragma once

#include <kairos_grid/grid_module.hpp>

#include "SurgeStorage.h"
#include "sst/filters/FilterCoefficientMaker.h"
#include "sst/filters/FilterConfiguration.h"
#include "sst/filters/QuadFilterUnit.h"
#include "sst/filters/TuningProvider.h"
#include "sst/utilities/globals.h"

#include <algorithm>
#include <cstring>

namespace kairos_grid {
namespace surge {

// Port layout for all SurgeFilterModule instances:
//   inputs[0]  : audio in L            (voltage, ±5 V nominal)
//   inputs[1]  : audio in R
//   inputs[2]  : cutoff frequency      (MIDI note; 69 = A4 = 440 Hz)
//   inputs[3]  : resonance             (0.0 – 1.0)
//   outputs[0] : audio out L
//   outputs[1] : audio out R
//
// Coefficients are re-computed once per BLOCK_SIZE (32) samples so that the
// filter state can apply the built-in coefficient-smoothing ramp on each call
// to filterPtr().

template <sst::filters::FilterType  FType,
          sst::filters::FilterSubType FSubType = sst::filters::FilterSubType::st_Standard>
class SurgeFilterModule : public GridModule {
public:
    static constexpr int kNumInputs  = 4;
    static constexpr int kNumOutputs = 2;

    SurgeFilterModule() : GridModule(kNumInputs, kNumOutputs) {}

    void prepare(const GridProcessArgs& args) override {
        tuning_.tuningApplicationMode =
            sst::filters::detail::BasicTuningProvider::TuningMode::RETUNE_ALL;

        coef_maker_.setSampleRateAndBlockSize(args.sample_rate, BLOCK_SIZE);
        coef_maker_.Reset();

        qfu_.sampleRate    = args.sample_rate;
        qfu_.sampleRateInv = 1.f / args.sample_rate;

        for (int i = 0; i < sst::filters::n_filter_registers; ++i)
            qfu_.R[i] = _mm_setzero_ps();
        for (int i = 0; i < sst::filters::n_cm_coeffs; ++i) {
            qfu_.C[i]  = _mm_setzero_ps();
            qfu_.dC[i] = _mm_setzero_ps();
        }

        for (int i = 0; i < 4; ++i) {
            qfu_.WP[i]     = 0;
            qfu_.active[i] = (i < 2) ? 0xFFFFFFFF : 0;
            qfu_.DB[i]     = delay_buf_[i];
        }
        std::memset(delay_buf_, 0, sizeof(delay_buf_));

        filter_ptr_ = sst::filters::GetQFPtrFilterUnit(FType, FSubType);
        if (!filter_ptr_)
            filter_ptr_ = pass_through;

        block_pos_ = BLOCK_SIZE;  // force coefficient update on first process()
    }

    void process(const GridProcessArgs&) override {
        if (block_pos_ >= BLOCK_SIZE) {
            const float cutoff = inputs[2].voltage;
            const float reso   = std::clamp(inputs[3].voltage, 0.f, 1.f);
            coef_maker_.MakeCoeffs(cutoff - 69.f, reso, FType, FSubType, &tuning_, false);
            coef_maker_.updateState(qfu_, -1);
            block_pos_ = 0;
        }

        // Surge normalises audio to ±1; our ports are ±5 V.
        alignas(16) const float in_arr[4] = {
            inputs[0].voltage * 0.2f,
            inputs[1].voltage * 0.2f,
            0.f, 0.f
        };
        const __m128 out = filter_ptr_(&qfu_, _mm_load_ps(in_arr));

        alignas(16) float out_arr[4];
        _mm_store_ps(out_arr, out);
        outputs[0].voltage = out_arr[0] * 5.f;
        outputs[1].voltage = out_arr[1] * 5.f;
        ++block_pos_;
    }

private:
    static __m128 pass_through(sst::filters::QuadFilterUnitState*, __m128 in) { return in; }

    static constexpr int kDelayBufSize =
        sst::filters::utilities::MAX_FB_COMB +
        sst::filters::utilities::SincTable::FIRipol_N;

    sst::filters::detail::BasicTuningProvider  tuning_{};
    sst::filters::FilterCoefficientMaker<
        sst::filters::detail::BasicTuningProvider>  coef_maker_;
    sst::filters::QuadFilterUnitState               qfu_{};
    sst::filters::FilterUnitQFPtr                   filter_ptr_{nullptr};
    float delay_buf_[4][kDelayBufSize]{};
    int   block_pos_{BLOCK_SIZE};
};

// ---------------------------------------------------------------------------
// Named aliases — most-used filter topologies
// ---------------------------------------------------------------------------

using namespace sst::filters;

using VintageLadderModule =
    SurgeFilterModule<FilterType::fut_vintageladder, FilterSubType::st_Standard>;
using DiodeLadderModule =
    SurgeFilterModule<FilterType::fut_diode, FilterSubType::st_Standard>;
using K35LPModule =
    SurgeFilterModule<FilterType::fut_k35_lp, FilterSubType::st_Standard>;
using K35HPModule =
    SurgeFilterModule<FilterType::fut_k35_hp, FilterSubType::st_Standard>;
using OBXD4PoleModule =
    SurgeFilterModule<FilterType::fut_obxd_4pole, FilterSubType::st_Standard>;
using LP12Module  = SurgeFilterModule<FilterType::fut_lp12>;
using LP24Module  = SurgeFilterModule<FilterType::fut_lp24>;
using HP12Module  = SurgeFilterModule<FilterType::fut_hp12>;
using HP24Module  = SurgeFilterModule<FilterType::fut_hp24>;
using BP12Module  = SurgeFilterModule<FilterType::fut_bp12>;
using BP24Module  = SurgeFilterModule<FilterType::fut_bp24>;

} // namespace surge
} // namespace kairos_grid
