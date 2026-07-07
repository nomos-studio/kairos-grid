// SPDX-License-Identifier: GPL-3.0-or-later
// Surge XT Effect wrappers as GridModules.

#pragma once

#include <kairos_grid/grid_module.hpp>

#include "Parameter.h"
#include "SurgeStorage.h"
#include "dsp/Effect.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace kairos_grid {
namespace surge {

    // Port layout for all SurgeEffectModule instances:
    //   inputs[0]          : audio in L            (voltage, ±5 V nominal)
    //   inputs[1]          : audio in R
    //   inputs[2..13]      : effect params 0..11   (0.0 – 1.0, scaled to param range)
    //   outputs[0]         : audio out L
    //   outputs[1]         : audio out R
    //
    // The effect processes BLOCK_SIZE (32) samples internally. One block of
    // latency (≈ 0.67 ms at 48 kHz) is introduced — acceptable for all
    // non-sample-sync uses. Parameters update once per block.
    //
    // For ct_none (unused) parameter slots, the input voltage is ignored.

    template <int FxType> class SurgeEffectModule : public GridModule {
      public:
        static constexpr int kNumInputs  = 2 + n_fx_params; // L, R, params 0-11
        static constexpr int kNumOutputs = 2;

        SurgeEffectModule() : GridModule(kNumInputs, kNumOutputs), fxdata_(fxslot_global1) {}

        ~SurgeEffectModule() override { destroy_effect(); }

        void prepare(const GridProcessArgs& args) override {
            destroy_effect();
            init_storage(args.sample_rate);
            spawn_and_init();
            std::memset(buf_L_, 0, sizeof(buf_L_));
            std::memset(buf_R_, 0, sizeof(buf_R_));
            block_pos_ = BLOCK_SIZE; // triggers processing on first process() call
        }

        void process(const GridProcessArgs&) override {
            if (block_pos_ >= BLOCK_SIZE) {
                update_params();
                effect_->process(buf_L_, buf_R_);
                block_pos_ = 0;
            }
            // Output the sample from the last processed block, then overwrite
            // that slot with the incoming sample for the next block.
            outputs[0].voltage = buf_L_[block_pos_] * 5.f;
            outputs[1].voltage = buf_R_[block_pos_] * 5.f;
            buf_L_[block_pos_] = inputs[0].voltage * 0.2f;
            buf_R_[block_pos_] = inputs[1].voltage * 0.2f;
            ++block_pos_;
        }

      private:
        void init_storage(float sample_rate) {
            SurgeStorage::SurgeStorageConfig cfg;
            cfg.suppliedDataPath    = SurgeStorage::skipPatchLoadDataPathSentinel;
            cfg.createUserDirectory = false;
            storage_                = std::make_unique<SurgeStorage>(cfg);
            storage_->setSamplerate(sample_rate); // also calls init_tables()

            // Assign sequential IDs so pd_float[i] maps to pd_[i].
            // FxStorage default-constructs all Parameters with id=0 which would
            // alias everything onto pd_[0] — we fix that here before spawning.
            for (int i = 0; i < n_fx_params; ++i)
                fxdata_.p[i].id = i;
            fxdata_.type.val.i = FxType;
        }

        void spawn_and_init() {
            std::memset(pd_, 0, sizeof(pd_));
            effect_ = spawn_effect(FxType, storage_.get(), &fxdata_, pd_);
            effect_->init_ctrltypes();
            effect_->init_default_values();
            // Copy defaults into pd_ so old-style effects (those not using
            // SurgeSSTFXBase) see the right initial values via pd_float[].
            for (int i = 0; i < n_fx_params; ++i)
                pd_[i] = fxdata_.p[i].val;
            effect_->init();
        }

        void destroy_effect() {
            delete effect_;
            effect_ = nullptr;
        }

        void update_params() {
            for (int i = 0; i < n_fx_params; ++i) {
                if (fxdata_.p[i].ctrltype == ct_none)
                    continue;
                const float v = std::clamp(inputs[2 + i].voltage, 0.f, 1.f);
                if (fxdata_.p[i].valtype == vt_float) {
                    const float lo = fxdata_.p[i].val_min.f;
                    const float hi = fxdata_.p[i].val_max.f;
                    pd_[i].f       = lo + v * (hi - lo);
                } else {
                    const int lo = fxdata_.p[i].val_min.i;
                    const int hi = fxdata_.p[i].val_max.i;
                    pd_[i].i     = static_cast<int>(std::round(lo + v * (hi - lo)));
                }
            }
        }

        std::unique_ptr<SurgeStorage> storage_;
        FxStorage                     fxdata_;
        pdata                         pd_[n_fx_params]{};
        Effect*                       effect_{nullptr};

        alignas(16) float buf_L_[BLOCK_SIZE]{};
        alignas(16) float buf_R_[BLOCK_SIZE]{};
        int block_pos_{BLOCK_SIZE};
    };

    // ---------------------------------------------------------------------------
    // Named aliases — common effects
    // ---------------------------------------------------------------------------

    using Reverb1Module    = SurgeEffectModule<fxt_reverb>;
    using Reverb2Module    = SurgeEffectModule<fxt_reverb2>;
    using ChorusModule     = SurgeEffectModule<fxt_chorus4>;
    using DelayModule      = SurgeEffectModule<fxt_delay>;
    using PhaserModule     = SurgeEffectModule<fxt_phaser>;
    using FlangerModule    = SurgeEffectModule<fxt_flanger>;
    using BonsaiModule     = SurgeEffectModule<fxt_bonsai>;
    using EnsembleModule   = SurgeEffectModule<fxt_ensemble>;
    using DistortionModule = SurgeEffectModule<fxt_distortion>;

} // namespace surge
} // namespace kairos_grid
