// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

// BridgeFrame — fixed-size shared memory frame for VCVRack ↔ kairos-grid IPC.
//
// One frame is exchanged per audio block in each direction.  The shared memory
// ring buffer (ShmRingBuffer) holds two slots, each sized sizeof(BridgeFrame).
//
// Layout:
//   Header fields (magic, sequence, config)
//   ES-8CV encoded CV channels (optional, 4 × f32 per direction = 8 × 12-bit CV)
//   ES-5 encoded gate bits (optional, 1 × f32 per direction = 8 gate bits)
//   Control sub-frame (optional EDN payload, nomos-rt IPC wire format verbatim)
//   Audio sample data (interleaved [channel][sample], both directions)
//
// Conventions:
//   "to_kairos" — data flowing from VCVRack toward kairos-grid
//   "to_vcv"    — data flowing from kairos-grid toward VCVRack
//   block_size  — number of audio samples per frame (≤ kBridgeBlockSize)
//   n_audio_*   — number of active audio channels (≤ kBridgeMaxChannels)
//
// Both processes must link against the same bridge_frame.hpp for layout agreement.

namespace kairos_grid::vcv_bridge {

constexpr uint32_t kBridgeMagic        = 0xBEEFCAFEu;
constexpr int      kBridgeMaxChannels  = 16;
constexpr int      kBridgeBlockSize    = 256;
constexpr int      kBridgeMaxCtrlBytes = 4096;

struct alignas(64) BridgeFrame {
    // --- header (64-byte cache line) ---
    uint32_t magic{kBridgeMagic};  // sanity check; invalid if != kBridgeMagic
    uint32_t sequence{0};          // monotonically increasing per producer commit
    uint32_t block_size{0};        // actual sample count this frame (≤ kBridgeBlockSize)
    uint32_t n_audio_to_kairos{0}; // active VCVRack→kairos audio channels
    uint32_t n_audio_to_vcv{0};    // active kairos→VCVRack audio channels
    uint32_t ctrl_len{0};          // byte length of ctrl_payload (0 = no control data)
    uint8_t  es8cv_valid{0};       // 1 = es8cv_* fields contain valid data
    uint8_t  es5_valid{0};         // 1 = es5_* fields contain valid data
    uint8_t  _pad[6]{};

    // --- ES-8CV (block-rate, optional) ---
    // 4 × f32 encodes 8 × 12-bit CV channels; use es8cv_encode/decode.
    float es8cv_to_kairos[4]{};
    float es8cv_to_vcv[4]{};

    // --- ES-5 (block-rate, optional) ---
    // 1 × f32 encodes 8 gate bits; use es5_encode/decode.
    float es5_to_kairos{0.0f};
    float es5_to_vcv{0.0f};

    // --- control sub-frame (optional) ---
    // EDN payload in the nomos-rt IPC wire format (8-byte hdr + EDN text).
    // Populated at most once per block; ctrl_len == 0 means no data.
    uint8_t ctrl_payload[kBridgeMaxCtrlBytes]{};

    // --- audio sample data (sample-rate) ---
    // Indexed [channel][sample].  Only [0..n_audio_to_kairos-1] / [0..block_size-1]
    // contain valid data; the remainder is zero-filled by the producer.
    float audio_to_kairos[kBridgeMaxChannels][kBridgeBlockSize]{};
    float audio_to_vcv[kBridgeMaxChannels][kBridgeBlockSize]{};
};

// kBridgeSlotSize: aligned size of one ring buffer slot.
constexpr std::size_t kBridgeSlotSize = sizeof(BridgeFrame);

} // namespace kairos_grid::vcv_bridge
