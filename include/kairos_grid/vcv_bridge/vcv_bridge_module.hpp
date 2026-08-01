// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <kairos_grid/grid_module.hpp>
#include <kairos_grid/vcv_bridge/bridge_frame.hpp>
#include <kairos_grid/vcv_bridge/es_codec.hpp>
#include <kairos_grid/vcv_bridge/shm_ring_buffer.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace kairos_grid::vcv_bridge {

// VCVBridgeModule — kairos-grid GridModule that bridges audio with a VCVRack plugin.
//
// Port layout (all counts = n_channels_):
//   outputs[0..n-1]  — audio FROM VCVRack delivered INTO the kairos graph.
//                      (source: shm_in_ audio_to_kairos[ch][sample_idx])
//   inputs[0..n-1]   — audio FROM the kairos graph sent BACK TO VCVRack.
//                      (sink:   shm_out_ audio_to_vcv[ch][sample_idx])
//
// Shared memory naming convention (shm_name passed to constructor):
//   shm_name + "-in"   — VCVRack produces, VCVBridgeModule consumes
//   shm_name + "-out"  — VCVBridgeModule produces, VCVRack consumes
//
// Block accumulation:
//   The kairos engine calls process() once per sample.  VCVBridgeModule
//   accumulates block_size_ samples before committing each out frame.
//   At the start of each block it consumes the latest in frame from VCVRack.
//   This results in one-block of round-trip latency — inherent to block IPC.
//
// Startup behaviour:
//   shm_in_ will fail to attach if VCVRack hasn't created the segment yet.
//   In that case consume() returns nullptr and the module outputs silence.
//   shm_out_ is created unconditionally; VCVRack attaches once it starts.
//   Both buffers tolerate late arrival of the other side without crashing.

class VCVBridgeModule : public GridModule {
  public:
    // Called when the ctrl sub-frame from VCVRack contains a non-empty IPC message.
    // header_and_payload: pointer to the raw 8-byte IPC header + payload.
    // total_len: total bytes (header + payload).
    // Called from the kairos audio thread (process()), so the handler must be fast
    // or dispatch to another thread.
    using CtrlInFn = std::function<void(const uint8_t* header_and_payload, uint32_t total_len)>;

    // shm_name: POSIX shm name prefix without leading '/' (e.g. "kairos-vcv-0").
    //           Two segments are created: "/<shm_name>-in" and "/<shm_name>-out".
    // n_channels: audio channels per direction (capped at kBridgeMaxChannels).
    // block_size: samples per exchange frame (capped at kBridgeBlockSize).
    VCVBridgeModule(const std::string& shm_name, int n_channels, int block_size = kBridgeBlockSize)
        : GridModule(clamp_channels(n_channels), clamp_channels(n_channels)),
          shm_in_(ShmRingBuffer::create_consumer("/" + shm_name + "-in")),
          shm_out_(ShmRingBuffer::create_producer("/" + shm_name + "-out")),
          n_channels_(clamp_channels(n_channels)),
          block_size_(std::clamp(block_size, 1, kBridgeBlockSize)) {}

    // Set the handler for inbound ctrl frames from VCVRack.
    // Not thread-safe — call before the kairos engine starts processing.
    void set_ctrl_in_fn(CtrlInFn fn) { ctrl_in_fn_ = std::move(fn); }

    // Push a ctrl response frame to VCVRack (written into the next out frame).
    // type: IPC message type byte (e.g. ipc::msg_repl_eval_response = 0x56).
    // payload: EDN response string.
    // Thread-safe — may be called from any thread (including the control thread).
    void push_ctrl_response(uint8_t type, std::string_view payload) {
        const uint32_t plen    = static_cast<uint32_t>(payload.size());
        const uint32_t plen_be = __builtin_bswap32(plen);
        const size_t   total   = 8 + payload.size();
        if (total > static_cast<size_t>(kBridgeMaxCtrlBytes))
            return;

        std::string frame(total, '\0');
        std::memcpy(frame.data(), &plen_be, 4);
        frame[4] = type;
        // frame[5..7] are reserved (already zero)
        if (!payload.empty())
            std::memcpy(frame.data() + 8, payload.data(), payload.size());

        std::lock_guard<std::mutex> lk(ctrl_out_mutex_);
        ctrl_out_pending_ = std::move(frame);
        ctrl_out_ready_.store(true, std::memory_order_release);
    }

    ~VCVBridgeModule() override = default;

    // Resets sample counter and sequence number on sample-rate change.
    void prepare(const GridProcessArgs& args) override {
        (void)args;
        sample_idx_ = 0;
        out_seq_    = 0;
        in_frame_   = nullptr;
        out_frame_  = nullptr; // obtained fresh at next block boundary
    }

    // Called once per sample by the kairos engine.
    //
    // At block boundaries:
    //   - commits the previous outgoing frame (if any)
    //   - consumes the latest incoming frame from VCVRack
    //   - claims a new writable outgoing frame
    //   - dispatches any ctrl sub-frame from VCVRack to ctrl_in_fn_
    //   - copies any pending ctrl response into the outgoing frame
    //
    // Per sample:
    //   - outputs[ch]  ← in_frame_->audio_to_kairos[ch][sample_idx_]  (or 0 if none)
    //   - out_frame_->audio_to_vcv[ch][sample_idx_] ← inputs[ch].voltage
    void process(const GridProcessArgs& /*args*/) override {
        // Block boundary: consume latest in frame and claim a fresh out slot.
        if (sample_idx_ == 0) {
            in_frame_  = shm_in_.consume();
            out_frame_ = shm_out_.writable_slot();
            if (out_frame_) {
                out_frame_->sequence          = ++out_seq_;
                out_frame_->block_size        = static_cast<uint32_t>(block_size_);
                out_frame_->n_audio_to_vcv    = static_cast<uint32_t>(n_channels_);
                out_frame_->n_audio_to_kairos = in_frame_ ? in_frame_->n_audio_to_kairos : 0u;
                out_frame_->ctrl_len          = 0;
            }

            // Dispatch inbound ctrl sub-frame from VCVRack (e.g. msg_repl_eval).
            if (in_frame_ && in_frame_->ctrl_len >= 8 && ctrl_in_fn_)
                ctrl_in_fn_(in_frame_->ctrl_payload, in_frame_->ctrl_len);

            // Copy any pending ctrl response into the outgoing frame.
            if (ctrl_out_ready_.load(std::memory_order_acquire) && out_frame_) {
                std::lock_guard<std::mutex> lk(ctrl_out_mutex_);
                if (ctrl_out_ready_.load(std::memory_order_relaxed)) {
                    const size_t sz = std::min(ctrl_out_pending_.size(),
                                               static_cast<size_t>(kBridgeMaxCtrlBytes));
                    std::memcpy(out_frame_->ctrl_payload, ctrl_out_pending_.data(), sz);
                    out_frame_->ctrl_len = static_cast<uint32_t>(sz);
                    ctrl_out_pending_.clear();
                    ctrl_out_ready_.store(false, std::memory_order_release);
                }
            }
        }

        // Deliver VCVRack audio into the kairos graph.
        for (int ch = 0; ch < n_channels_; ++ch) {
            outputs[static_cast<std::size_t>(ch)].voltage =
                in_frame_ ? in_frame_->audio_to_kairos[ch][sample_idx_] : 0.f;
        }

        // Accumulate kairos graph audio for VCVRack.
        if (out_frame_) {
            for (int ch = 0; ch < n_channels_; ++ch) {
                out_frame_->audio_to_vcv[ch][sample_idx_] =
                    inputs[static_cast<std::size_t>(ch)].voltage;
            }
        }

        // Commit when the block is full (end of block, not start of next).
        if (++sample_idx_ >= block_size_) {
            sample_idx_ = 0;
            shm_out_.commit();
        }
    }

    // Accessors for tests and diagnostics.
    bool shm_in_valid() const noexcept { return shm_in_.valid(); }
    bool shm_out_valid() const noexcept { return shm_out_.valid(); }
    int  n_channels() const noexcept { return n_channels_; }
    int  block_size() const noexcept { return block_size_; }

  private:
    static int clamp_channels(int n) noexcept { return std::clamp(n, 1, kBridgeMaxChannels); }

    ShmRingBuffer shm_in_;  // consumer of VCVRack → kairos stream
    ShmRingBuffer shm_out_; // producer of kairos → VCVRack stream
    int           n_channels_;
    int           block_size_;
    int           sample_idx_{0};
    uint32_t      out_seq_{0};

    const BridgeFrame* in_frame_{nullptr};  // latest frame from VCVRack
    BridgeFrame*       out_frame_{nullptr}; // outgoing frame being built

    // Ctrl sub-frame routing.
    CtrlInFn          ctrl_in_fn_;            // optional; set before engine starts
    std::mutex        ctrl_out_mutex_;        // guards ctrl_out_pending_
    std::string       ctrl_out_pending_;      // next response to write into out_frame_
    std::atomic<bool> ctrl_out_ready_{false}; // lock-free dirty flag
};

} // namespace kairos_grid::vcv_bridge
