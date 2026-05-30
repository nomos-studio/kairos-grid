// SPDX-License-Identifier: GPL-3.0-or-later
//
// kairos/patch-bus — custom CLAP extension.
//
// Host pushes an EDN patch descriptor string; the plugin atomically rebuilds
// its GridEngine at the next process() block boundary.  The param-bus and
// tap-bus schemas are invalidated and updated as part of the same atomic swap.
//
// This is a plain C header shared verbatim between kairos and kairos-grid.
// Copy it to both source trees; keep the copies in sync (ABI is stable within
// a major version).
//
// Protocol:
//   1. Host queries get_extension(plugin, CLAP_EXT_KAIROS_PATCH_BUS) after
//      create_plugin(); result is non-null if the plugin supports it.
//   2. Host calls push_patch() from the main thread with an EDN descriptor.
//      Returns false if the descriptor is null, zero-length, or invalid.
//   3. The plugin schedules a lock-free engine swap; the swap occurs at the
//      start of the next process() call on the audio thread.  The previous
//      engine is torn down on the audio thread (single dealloc, infrequent).
//   4. Host calls get_patch() to retrieve the most recently accepted descriptor.
//      After push_patch() returns true, get_patch() immediately reflects the
//      new descriptor (no need to wait for the audio-thread swap).
//   5. Host must re-query param-bus and tap-bus schemas after any push_patch()
//      to see the new schema, since the engine may have a different set of ports.
//
// Thread safety:
//   push_patch()  — main thread only.
//   get_patch()   — main thread only.

#pragma once

#include <clap/clap.h>
#include <stdint.h>

#define CLAP_EXT_KAIROS_PATCH_BUS "kairos/patch-bus"

// Plugin-side vtable returned by get_extension(CLAP_EXT_KAIROS_PATCH_BUS).
typedef struct {
    // Push an EDN patch descriptor to the plugin.
    // Parses the descriptor and schedules a lock-free engine swap at the next
    // process() block boundary.  The param-bus and tap-bus schemas are rebuilt
    // as part of the same atomic swap.
    // Returns false if: edn_descriptor is null, len is 0, the descriptor
    // contains an unknown module type, or the graph has a cycle.
    // Called from the main thread only.
    bool (CLAP_ABI *push_patch)(const clap_plugin_t* plugin,
                                const char* edn_descriptor, uint32_t len);

    // Return the EDN descriptor most recently accepted by push_patch().
    // Returns null until push_patch() has been called with a valid descriptor.
    // The returned pointer is valid until the next push_patch() call.
    // Called from the main thread only.
    const char* (CLAP_ABI *get_patch)(const clap_plugin_t* plugin);
} clap_plugin_patch_bus_t;
