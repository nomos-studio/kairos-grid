// SPDX-License-Identifier: GPL-3.0-or-later
//
// kairos/hot-swap — custom CLAP extension for gapless WASM DSP hot-swap.
//
// Allows the host to request a new .wasm file for a running plugin without
// stopping audio.  The plugin replaces the WASM DSP module at the next
// process() block boundary, preserving audio continuity.
//
// ABI contract (must match kairos/src/wasm_bridge_plugin.hpp):
//   Extension ID : "org.cljseq.kairos.ext.hot-swap/1"
//   Struct layout: one function pointer (request).
//
// Protocol:
//   1. Host queries get_extension(plugin, CLAP_EXT_KAIROS_HOT_SWAP).
//      Returns non-null only when the plugin was built with WASM support.
//   2. Host calls request() from the main thread with the new .wasm path.
//      Returns true if the path is readable and the swap was queued.
//      Returns false if the path is unreadable, the plugin has no WASM slot,
//      or the new module has a different audio topology (input/output count).
//   3. The plugin swaps the WASM module at the next process() block boundary.
//      Param values are migrated by label; unmatched params use Faust defaults.
//
// Thread safety:
//   request() — main thread only.
//
// This is a plain C header shared verbatim between kairos and kairos-grid.
// Keep copies in sync; the ABI is stable within the major version.

#pragma once

#include <clap/clap.h>

#define CLAP_EXT_KAIROS_HOT_SWAP "org.cljseq.kairos.ext.hot-swap/2"

// Plugin-side vtable returned by get_extension(CLAP_EXT_KAIROS_HOT_SWAP).
typedef struct {
    // Queue new_wasm_path for the next block-boundary swap.
    //
    // new_wasm_path — replacement .wasm file (must not be null or empty).
    // old_wasm_path — identifies which WASM slot to replace, by its current
    //                 .wasm path.  NULL or empty = replace the first WASM slot
    //                 (backward-compatible behaviour for single-slot patches).
    //
    // Returns false if: new_wasm_path is null/empty, the file is unreadable,
    // no WASM patch is currently loaded, or the new module has different I/O counts.
    // Called from the main thread only.
    bool (CLAP_ABI *request)(const clap_plugin_t* plugin,
                             const char* new_wasm_path,
                             const char* old_wasm_path);
} clap_kairos_hot_swap_t;
