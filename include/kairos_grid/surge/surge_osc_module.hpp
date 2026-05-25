// SPDX-License-Identifier: GPL-3.0-or-later
//
// SurgeOscModule<OscType> — GridModule wrapper around a Surge XT oscillator.
//
// Port layout (matches PlaitsModule convention):
//   inputs[0]  : MIDI note (float; 60 = C4, 69 = A4 / 440 Hz)
//   inputs[1]  : param p[0] — oscillator-specific (0..1)
//   ...
//   inputs[7]  : param p[6]
//   outputs[0] : left / mono
//   outputs[1] : right
//
// Surge oscillators run at 2× oversampling (BLOCK_SIZE_OS = 2 * BLOCK_SIZE).
// process() buffers BLOCK_SIZE_OS samples per block and serves them one at a
// time via a halfband decimator, exactly as surge-rack's VCO does.
//
// Oscillator type is selected at compile time via the OscType template
// parameter (values from the osc_type enum: ot_classic, ot_sine, etc.).

#pragma once

#include <kairos_grid/grid_module.hpp>

// Surge headers — available through the kairos_grid::surge-modules target.
#include <SurgeStorage.h>
#include <globals.h>
#include <dsp/Oscillator.h>
#include <sst/filters/HalfRateFilter.h>
#include <sst/basic-blocks/mechanics/block-ops.h>

#include <memory>
#include <cstring>
#include <algorithm>

namespace kairos_grid {
namespace surge {

template <int OscType>
class SurgeOscModule : public GridModule {
  public:
    static constexpr int kNumInputs  = 1 + n_osc_params;  // note + 7 params
    static constexpr int kNumOutputs = 2;

    SurgeOscModule() : GridModule(kNumInputs, kNumOutputs) {}

    ~SurgeOscModule() override
    {
        destroy_osc();
    }

    void prepare(const GridProcessArgs& args) override
    {
        destroy_osc();
        init_storage(args.sample_rate);
        halfband_.reset();
        process_pos_ = BLOCK_SIZE + 1;
    }

    void process(const GridProcessArgs&) override
    {
        if (process_pos_ >= BLOCK_SIZE)
        {
            // Lazily spawn on first call after prepare().
            if (!surge_osc_)
                spawn_and_init();

            const float note = inputs[0].voltage;
            for (int i = 0; i < n_osc_params; ++i)
                oscstorage_->p[i].set_value_f01(std::clamp(inputs[1 + i].voltage, 0.f, 1.f));

            copy_scene_data();
            surge_osc_->process_block(note, 0.f, /*stereo=*/true);

            // Decimate 2× oversampled output in-place; first BLOCK_SIZE
            // elements of each array hold the result.
            sst::basic_blocks::mechanics::copy_from_to<BLOCK_SIZE_OS>(
                surge_osc_->output,  buf_L_);
            sst::basic_blocks::mechanics::copy_from_to<BLOCK_SIZE_OS>(
                surge_osc_->outputR, buf_R_);
            halfband_.process_block_D2(buf_L_, buf_R_, BLOCK_SIZE_OS);

            process_pos_ = 0;
        }

        outputs[0].voltage = buf_L_[process_pos_] * 5.f;
        outputs[1].voltage = buf_R_[process_pos_] * 5.f;
        ++process_pos_;
    }

  private:
    void init_storage(float sample_rate)
    {
        SurgeStorage::SurgeStorageConfig cfg;
        cfg.suppliedDataPath    = SurgeStorage::skipPatchLoadDataPathSentinel;
        cfg.createUserDirectory = false;

        storage_ = std::make_unique<SurgeStorage>(cfg);
        storage_->getPatch().init_default_values();
        storage_->getPatch().copy_globaldata(storage_->getPatch().globaldata);
        storage_->getPatch().copy_scenedata(storage_->getPatch().scenedata[0], 0);
        storage_->getPatch().copy_scenedata(storage_->getPatch().scenedata[1], 1);
        storage_->setSamplerate(sample_rate);
        storage_->init_tables();

        oscstorage_ = &storage_->getPatch().scene[0].osc[0];
        oscstorage_->type.val.i = OscType;
        setup_storage_ranges();
    }

    void spawn_and_init()
    {
        surge_osc_ = spawn_osc(OscType, storage_.get(), oscstorage_,
                               storage_->getPatch().scenedata[0], oscbuf_);
        surge_osc_->init_ctrltypes();
        surge_osc_->init_default_values();
        surge_osc_->init_extra_config();
        surge_osc_->init(60.f);
    }

    void destroy_osc()
    {
        if (surge_osc_)
        {
            surge_osc_->~Oscillator();
            surge_osc_ = nullptr;
        }
    }

    void setup_storage_ranges()
    {
        int lo = 100000, hi = -1;
        for (Parameter* p = &oscstorage_->type; p <= &oscstorage_->retrigger; ++p)
        {
            if (p->id >= 0)
            {
                lo = std::min(lo, p->id);
                hi = std::max(hi, p->id);
            }
        }
        storage_id_start_ = lo;
        storage_id_end_   = hi + 1;
    }

    void copy_scene_data()
    {
        const int s = storage_->getPatch().scene_start[0];
        for (int i = storage_id_start_; i < storage_id_end_; ++i)
            storage_->getPatch().scenedata[0][i - s].i =
                storage_->getPatch().param_ptr[i]->val.i;
    }

    std::unique_ptr<SurgeStorage>           storage_;
    Oscillator*                             surge_osc_{nullptr};
    OscillatorStorage*                      oscstorage_{nullptr};

    alignas(16) unsigned char               oscbuf_[oscillator_buffer_size];
    alignas(16) float                       buf_L_[BLOCK_SIZE_OS];
    alignas(16) float                       buf_R_[BLOCK_SIZE_OS];

    sst::filters::HalfRate::HalfRateFilter  halfband_{6, true};

    int process_pos_{BLOCK_SIZE + 1};
    int storage_id_start_{0};
    int storage_id_end_{0};
};

// Convenience aliases for the most common non-wavetable oscillators.
using ClassicOscModule = SurgeOscModule<ot_classic>;
using SineOscModule    = SurgeOscModule<ot_sine>;
using ModernOscModule  = SurgeOscModule<ot_modern>;
using FM2OscModule     = SurgeOscModule<ot_FM2>;
using FM3OscModule     = SurgeOscModule<ot_FM3>;
using StringOscModule  = SurgeOscModule<ot_string>;
using TwistOscModule   = SurgeOscModule<ot_twist>;

} // namespace surge
} // namespace kairos_grid
