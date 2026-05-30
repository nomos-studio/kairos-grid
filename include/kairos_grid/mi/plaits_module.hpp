// SPDX-License-Identifier: GPL-3.0-or-later
// Plaits macro-oscillator (MI) wrapped as a GridModule.
//
// Plaits::Voice::Render() operates at a fixed kSampleRate (48000 Hz) in
// blocks of kBlockSize (12) samples.  This wrapper maintains an internal
// frame buffer and refills it on demand, so the GridEngine sees a plain
// per-sample module with no special-casing.
//
// Inputs:
//   0  note       — MIDI note number (float, e.g. 69.0 = A4)
//   1  harmonics  — 0-1
//   2  timbre     — 0-1
//   3  morph      — 0-1
//   4  trigger    — gate (>0 triggers the internal envelope)
//   5  level      — 0-1  (LPG level / amplitude)
//   6  engine     — 0-15 (oscillator model; rounded to nearest integer)
// Outputs:
//   0  out        — main output  (int16 / 32768.f)
//   1  aux        — auxiliary output

#pragma once

#include <kairos_grid/grid_module.hpp>

// stmlib uses ARM-target inline assembly that clang flags on Apple Silicon host
// builds; suppress those diagnostics when including the MI headers.
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wasm-operand-widths"
#  pragma clang diagnostic ignored "-Wunused-parameter"
#  pragma clang diagnostic ignored "-Wunused-local-typedef"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#  pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#endif
#include <plaits/dsp/voice.h>
#include <stmlib/utils/buffer_allocator.h>
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

namespace kairos_grid::mi {

class PlaitsModule : public GridModule {
  public:
    PlaitsModule() : GridModule(7, 2) {
        allocator_.Init(memory_, sizeof(memory_));
        voice_.Init(&allocator_);

        patch_           = {};
        patch_.note      = 60.f;
        patch_.harmonics = 0.5f;
        patch_.timbre    = 0.5f;
        patch_.morph     = 0.5f;
        patch_.decay     = 0.5f;
        patch_.lpg_colour = 0.f;
        patch_.engine    = 0;

        mods_                    = {};
        mods_.trigger_patched    = true;
        mods_.level_patched      = true;

        buf_idx_ = plaits::kBlockSize; // force refill on first process()
    }

    void process(const GridProcessArgs&) override {
        if (buf_idx_ >= static_cast<int>(plaits::kBlockSize)) {
            patch_.note      = inputs[0].voltage;
            patch_.harmonics = clamp01(inputs[1].voltage);
            patch_.timbre    = clamp01(inputs[2].voltage);
            patch_.morph     = clamp01(inputs[3].voltage);
            patch_.engine    = static_cast<int>(inputs[6].voltage + 0.5f);
            if (patch_.engine < 0)  patch_.engine = 0;
            if (patch_.engine > 15) patch_.engine = 15;

            mods_.trigger = inputs[4].voltage;
            mods_.level   = clamp01(inputs[5].voltage);

            voice_.Render(patch_, mods_, frames_, plaits::kBlockSize);
            buf_idx_ = 0;
        }

        const auto& f = frames_[buf_idx_++];
        outputs[0].voltage = static_cast<float>(f.out)  / 32768.f;
        outputs[1].voltage = static_cast<float>(f.aux) / 32768.f;
    }

  private:
    static float clamp01(float v) noexcept {
        if (v < 0.f) return 0.f;
        if (v > 1.f) return 1.f;
        return v;
    }

    alignas(4) char memory_[16384];
    stmlib::BufferAllocator allocator_;
    plaits::Voice           voice_;
    plaits::Patch           patch_;
    plaits::Modulations     mods_;
    plaits::Voice::Frame    frames_[plaits::kBlockSize];
    int                     buf_idx_;
};

} // namespace kairos_grid::mi
