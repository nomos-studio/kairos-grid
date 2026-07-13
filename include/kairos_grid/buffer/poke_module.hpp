// SPDX-License-Identifier: GPL-3.0-or-later
// poke — CV-gated buffer write; the write half of the CV-addressable buffer archetype.
//
// Self-contained: owns two internal float buffers (L and R) and provides an immediate
// linear readback at an independent read-pos.  This makes poke a complete live-capture +
// playback unit without requiring a separate storage primitive or buffer-sharing
// infrastructure between modules.
//
// The read side is always linear interpolation.  Use peek (with its configurable kernel)
// for the wavetable oscillator role; use poke for live capture where write-before-read
// semantics are needed.
//
// Write semantics: write-before-read each block.
//   gate > 0.5 → write in-l / in-r to floor(write-pos × (N-1)), clamped.
//   Output reflects the write in the same block it occurs.
//
// Ports (5 inputs, 2 outputs):
//   inputs[0]  in-l       — signal to write (left)
//   inputs[1]  in-r       — signal to write (right)
//   inputs[2]  write-pos  — CV [0, 1]; where to write; clamped
//   inputs[3]  read-pos   — CV [0, 1]; where to read; clamped; independent of write-pos
//   inputs[4]  gate       — write enable; > 0.5 = write this block
//   outputs[0] out-l      — linear read at read-pos (left)
//   outputs[1] out-r      — linear read at read-pos (right)
//
// Buffer layout:
//   Two flat float vectors, buffer_l_ and buffer_r_, one per channel.
//   Default size 2048 (2^11) matching peek's default; set at construction.
//   buffer_l() / buffer_r() return non-const references for external inspection
//   and pre-fill (fill_* helpers from PeekModule work on these too).
//
// Relationship to peek:
//   peek reads a buffer it owns; poke writes a buffer it owns.  Full poke → peek
//   shared-memory composition is blocked on a named buffer-slot registry in GridGraph
//   (the future `buffer` primitive).  Until then, poke's readback port covers live
//   capture + immediate playback; peek covers static wavetable oscillation.
//
// Param bus paths (registered in clap_plugin.cpp):
//   <prefix>/write-pos → input index 2
//   <prefix>/read-pos  → input index 3
//   <prefix>/gate      → input index 4

#pragma once

#include <kairos_grid/grid_module.hpp>

#include <algorithm>
#include <memory>
#include <vector>

namespace kairos_grid {

class PokeModule : public GridModule {
  public:
    // Standalone: allocates its own L and R buffers.
    explicit PokeModule(std::size_t size = 2048)
        : GridModule(5, 2), buf_l_ptr_(std::make_shared<std::vector<float>>(size, 0.f)),
          buf_r_ptr_(std::make_shared<std::vector<float>>(size, 0.f)) {}

    // Shared: receives pre-created buffers (from BufferModule or the buf_id factory).
    PokeModule(std::shared_ptr<std::vector<float>> l, std::shared_ptr<std::vector<float>> r)
        : GridModule(5, 2), buf_l_ptr_(std::move(l)), buf_r_ptr_(std::move(r)) {}

    std::vector<float>&       buffer_l() noexcept { return *buf_l_ptr_; }
    const std::vector<float>& buffer_l() const noexcept { return *buf_l_ptr_; }

    std::vector<float>&       buffer_r() noexcept { return *buf_r_ptr_; }
    const std::vector<float>& buffer_r() const noexcept { return *buf_r_ptr_; }

    void process(const GridProcessArgs&) override {
        const bool gate = inputs[4].voltage > 0.5f;

        // Write before read so output reflects this block's capture.
        if (gate) {
            const float       wpos = std::clamp(inputs[2].voltage, 0.f, 1.f);
            const std::size_t wi   = write_index(wpos);
            (*buf_l_ptr_)[wi]      = inputs[0].voltage;
            (*buf_r_ptr_)[wi]      = inputs[1].voltage;
        }

        const float rpos   = std::clamp(inputs[3].voltage, 0.f, 1.f);
        outputs[0].voltage = read_linear(*buf_l_ptr_, rpos);
        outputs[1].voltage = read_linear(*buf_r_ptr_, rpos);
    }

  private:
    std::shared_ptr<std::vector<float>> buf_l_ptr_;
    std::shared_ptr<std::vector<float>> buf_r_ptr_;

    std::size_t write_index(float pos) const noexcept {
        const float       pos_f = pos * static_cast<float>(buf_l_ptr_->size() - 1);
        const std::size_t i     = static_cast<std::size_t>(pos_f);
        return std::min(i, buf_l_ptr_->size() - 1);
    }

    static float read_linear(const std::vector<float>& buf, float pos) noexcept {
        const float       pos_f = pos * static_cast<float>(buf.size() - 1);
        const std::size_t i0    = static_cast<std::size_t>(pos_f);
        const std::size_t i1    = std::min(i0 + 1, buf.size() - 1);
        const float       frac  = pos_f - static_cast<float>(i0);
        return buf[i0] + frac * (buf[i1] - buf[i0]);
    }
};

} // namespace kairos_grid
