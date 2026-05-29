// SPDX-License-Identifier: GPL-3.0-or-later
//
// kairos/tap-bus — custom CLAP extension.
//
// Exposes the GridEngine's performance tap schema and per-block tap values from
// kairos-grid (plugin/provider) to kairos (host/consumer) at block rate.
//
// This is a plain C header so it can be shared verbatim between the two repos
// without creating a build-time dependency. Copy it to the kairos source tree
// and keep the two copies in sync (the ABI is stable within a major version).
//
// Protocol:
//   1. Host queries get_extension(plugin, CLAP_EXT_KAIROS_TAP_BUS) after
//      create_plugin(); result is non-null if the plugin supports it.
//   2. After activate() or reset(), host calls get_schema() to learn which taps
//      exist and their stable session IDs.  Re-query on any epoch change.
//   3. After each process() call, host calls get_tap_frame() to read current
//      tap values.  tap_frame[entry.id] is the value for that entry.
//
// Thread safety:
//   get_schema()    — audio thread or main thread; schema is stable until the
//                     next activate() or reset() invalidates it.
//   get_tap_frame() — audio thread only; valid immediately after process().

#pragma once

#include <clap/clap.h>
#include <stdint.h>

#define CLAP_EXT_KAIROS_TAP_BUS "kairos/tap-bus"

// One performance tap entry.
// All fields are valid until the next activate() or reset().
typedef struct {
    uint32_t    id;    // stable index into tap_frame[] for this session
    const char* name;  // e.g. "signal/envelope" — UTF-8, null-terminated
} clap_kairos_tap_entry_t;

// Snapshot of all performance taps registered at the last activate() call.
// epoch increments every time the schema changes (activate or reset).
typedef struct {
    uint32_t                       epoch;    // schema generation counter
    uint32_t                       count;    // number of valid entries
    const clap_kairos_tap_entry_t* entries;  // [count] entries; null when count == 0
} clap_kairos_tap_schema_t;

// Plugin-side vtable returned by get_extension(CLAP_EXT_KAIROS_TAP_BUS).
typedef struct {
    // Return the current tap schema.
    // The returned pointer is owned by the plugin and is valid until the next
    // activate() or reset() call.  Never returns null after a successful
    // activate().
    const clap_kairos_tap_schema_t* (CLAP_ABI *get_schema)(const clap_plugin_t* plugin);

    // Return tap values from the most recent process() call.
    // *out_count is set to schema->count.
    // Returns null (and sets *out_count = 0) if the engine has no taps or has
    // not yet been activated.
    // The pointer is valid until the next process() call on the audio thread.
    const float* (CLAP_ABI *get_tap_frame)(const clap_plugin_t* plugin, uint32_t* out_count);
} clap_plugin_tap_bus_t;
