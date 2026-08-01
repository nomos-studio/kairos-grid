// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <clap/plugin.h>
#include <stdint.h>

// kairos/vcv-ctrl — extension for pushing ctrl responses to VCVRack.
//
// Exposed by the kairos-grid CLAP plugin when a patch containing a
// "vcv-bridge" module with shm_name "kairos-vcv-tty" is loaded.
//
// The kairos host queries this extension after msg_graph_load and
// wires rt_control_thread::config.tty_sink to push_ctrl_response.
// This allows :vcvrack-tty eval results (e.g. from MCP) to propagate
// back to the TTY screen in VCVRack via the shm ring buffer.

#define CLAP_EXT_KAIROS_VCV_CTRL "kairos/vcv-ctrl"

typedef struct clap_kairos_vcv_ctrl {
    // Thread-safe: stage a ctrl response for delivery to the TTY VCVBridgeModule.
    // type:        IPC message type byte (0x56 = msg_repl_eval_response).
    // payload:     EDN response bytes (not null-terminated; use payload_len).
    // payload_len: byte count of payload.
    // Silently dropped if no TTY bridge is in the current patch.
    void (*push_ctrl_response)(const clap_plugin_t* plugin, uint8_t type, const char* payload,
                               uint32_t payload_len);
} clap_kairos_vcv_ctrl_t;
