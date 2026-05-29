// SPDX-License-Identifier: GPL-3.0-or-later
//
// kairos/param-bus — custom CLAP extension.
//
// Exposes the GridEngine's param-bus port schema and provides a direct
// frame-write path from kairos (host/consumer) to kairos-grid (plugin/provider)
// at block rate.  Symmetric counterpart to kairos/tap-bus.
//
// This is a plain C header so it can be shared verbatim between the two repos
// without creating a build-time dependency. Copy it to the kairos source tree
// and keep the two copies in sync (the ABI is stable within a major version).
//
// Protocol:
//   1. Host queries get_extension(plugin, CLAP_EXT_KAIROS_PARAM_BUS) after
//      create_plugin(); result is non-null if the plugin supports it.
//   2. After activate() or reset(), host calls get_schema() to learn which
//      param-bus ports exist and their stable session IDs.  Re-query on any
//      epoch change.
//   3. Before each process() call, host calls set_param_frame() to push the
//      current param values.  values[i] maps to schema->entries[i].
//      CLAP_EVENT_PARAM_VALUE events arriving inside process() may overwrite
//      individual entries (DAW automation takes precedence for the same port).
//
// Thread safety:
//   get_schema()      — audio thread or main thread; schema is stable until the
//                       next activate() or reset() invalidates it.
//   set_param_frame() — audio thread only; must be called before process().

#pragma once

#include <clap/clap.h>
#include <stdint.h>

#define CLAP_EXT_KAIROS_PARAM_BUS "kairos/param-bus"

// One param-bus port entry.
// All fields are valid until the next activate() or reset().
typedef struct {
    uint32_t    id;    // stable index into the param frame for this session
    const char* name;  // e.g. "env/tempo_hz" — UTF-8, null-terminated
} clap_kairos_param_entry_t;

// Snapshot of all param-bus ports registered at the last activate() call.
// epoch increments every time the schema changes (activate or reset).
typedef struct {
    uint32_t                          epoch;    // schema generation counter
    uint32_t                          count;    // number of valid entries
    const clap_kairos_param_entry_t*  entries;  // [count] entries; null when count == 0
} clap_kairos_param_schema_t;

// Plugin-side vtable returned by get_extension(CLAP_EXT_KAIROS_PARAM_BUS).
typedef struct {
    // Return the current param schema.
    // The returned pointer is owned by the plugin and is valid until the next
    // activate() or reset() call.  Never returns null after a successful
    // activate().
    const clap_kairos_param_schema_t* (CLAP_ABI *get_schema)(const clap_plugin_t* plugin);

    // Write param values directly into the plugin's param frame.
    // values[i] corresponds to schema->entries[i]; entries with i >= count are
    // left unchanged.  Extra values beyond schema->count are ignored.
    // Must be called from the audio thread before each process() call.
    // Returns false if the plugin is not activated or the schema is empty.
    bool (CLAP_ABI *set_param_frame)(const clap_plugin_t* plugin,
                                      const float* values, uint32_t count);
} clap_plugin_param_bus_t;
