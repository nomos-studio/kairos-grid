// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <kairos_grid/grid_module.hpp>

#include <algorithm>
#include <cstdint>

namespace kairos_grid {

// ---------------------------------------------------------------------------
// AudioInputModule — feeds CLAP audio input buffers into the grid.
//
// Before each step_block(N) call, the CLAP plugin writes the host's audio
// input buffer via set_buffers(). Each process() call reads one sample from
// that buffer (L = output 0, R = output 1) and advances the internal frame
// counter. If set_buffers() was not called (e.g. no audio input port), both
// outputs remain 0.
//
// Outputs:
//   0  left    sample value from channel 0
//   1  right   sample value from channel 1 (or 0 if source is mono/absent)
// ---------------------------------------------------------------------------
class AudioInputModule : public GridModule {
  public:
    enum Output {
        k_left        = 0,
        k_right       = 1,
        k_num_outputs = 2,
    };

    AudioInputModule() : GridModule(0, k_num_outputs) {}

    // Called by the CLAP host bridge before step_block().
    // bufs[c] is the channel-c float buffer; n_ch is the source channel count.
    void set_buffers(float* const* bufs, uint32_t n_ch, uint32_t n_frames) noexcept {
        bufs_     = bufs;
        n_ch_     = n_ch;
        n_frames_ = n_frames;
        frame_    = 0;
    }

    void prepare(const GridProcessArgs&) override {
        bufs_  = nullptr;
        n_ch_  = 0;
        frame_ = 0;
    }

    void process(const GridProcessArgs&) override {
        for (uint32_t c = 0; c < k_num_outputs; ++c) {
            outputs[c].voltage =
                (bufs_ && c < n_ch_ && frame_ < n_frames_) ? bufs_[c][frame_] : 0.f;
        }
        ++frame_;
    }

  private:
    float* const* bufs_{nullptr};
    uint32_t      n_ch_{0};
    uint32_t      n_frames_{0};
    uint32_t      frame_{0};
};

// ---------------------------------------------------------------------------
// AudioOutputModule — drains grid signals into CLAP audio output buffers.
//
// Before each step_block(N) call, the CLAP plugin writes the host's audio
// output buffer via set_buffers(). Each process() call writes one sample from
// inputs[0]/[1] into the buffer (L = input 0, R = input 1) and advances the
// internal frame counter. Extra output channels (beyond 2) are zeroed.
// If set_buffers() was not called, writes are silently skipped.
//
// Inputs:
//   0  left    written to channel 0
//   1  right   written to channel 1
// ---------------------------------------------------------------------------
class AudioOutputModule : public GridModule {
  public:
    enum Input {
        k_left       = 0,
        k_right      = 1,
        k_num_inputs = 2,
    };

    AudioOutputModule() : GridModule(k_num_inputs, 0) {}

    // Called by the CLAP host bridge before step_block().
    void set_buffers(float** bufs, uint32_t n_ch, uint32_t n_frames) noexcept {
        bufs_     = bufs;
        n_ch_     = n_ch;
        n_frames_ = n_frames;
        frame_    = 0;
    }

    void prepare(const GridProcessArgs&) override {
        bufs_  = nullptr;
        n_ch_  = 0;
        frame_ = 0;
    }

    void process(const GridProcessArgs&) override {
        if (!bufs_ || frame_ >= n_frames_) {
            ++frame_;
            return;
        }
        for (uint32_t c = 0; c < k_num_inputs; ++c)
            if (c < n_ch_)
                bufs_[c][frame_] = inputs[c].voltage;
        for (uint32_t c = k_num_inputs; c < n_ch_; ++c)
            bufs_[c][frame_] = 0.f;
        ++frame_;
    }

  private:
    float**  bufs_{nullptr};
    uint32_t n_ch_{0};
    uint32_t n_frames_{0};
    uint32_t frame_{0};
};

} // namespace kairos_grid
