// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <kairos_grid/grid_module.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace kairos_grid {

// One entry in the tap schema — identifies a single performance tap by its
// stable per-session ID and the module/tap indices needed to locate it.
struct TapEntry {
    int         id;          // index into the flat tap_frame() array
    std::string name;        // e.g. "signal/envelope"
    int         module_idx;  // which module owns this tap
    int         tap_idx;     // index into that module's taps[] vector
};

// Snapshot of all performance taps registered at the last prepare() call.
// Stable within a session; epoch increments on every prepare().
struct TapSchema {
    std::vector<TapEntry> entries;
    uint32_t              epoch{0};
    int size() const noexcept { return static_cast<int>(entries.size()); }
};

// One entry in the port schema — identifies a named param-bus input port.
struct PortEntry {
    int         id;          // index into the apply_params() frame
    std::string name;        // e.g. "env/cutoff"
    int         module_idx;  // which module owns this port
    int         port_idx;    // index into that module's inputs[] vector
};

// Snapshot of all named param-bus ports registered at the last prepare() call.
struct PortSchema {
    std::vector<PortEntry> entries;
    uint32_t               epoch{0};
    int size() const noexcept { return static_cast<int>(entries.size()); }
};

// A directed connection from one module's output port to another's input port.
//
// After all modules process() in a sample, the engine copies output voltages to
// input voltages along each cable. Multiple cables to the same input port are
// not summed — the last cable in the list wins. Mixing must be done explicitly
// via a mix GridModule.
struct GridCable {
    int from_module{-1};
    int from_port{-1};
    int to_module{-1};
    int to_port{-1};
};

// The sample-rate engine.
//
// Owns a set of GridModules (via unique_ptr), a cable list, and a pre-computed
// topological order. Constructed by GridGraph::build(); not directly
// constructible from user code.
//
// Usage per CLAP process() call:
//   1. Write CLAP audio input buffers to "source" module output ports.
//   2. Call step_block(n_frames).
//   3. Read "sink" module input ports to fill CLAP audio output buffers.
class GridEngine {
  public:
    GridEngine(std::vector<std::unique_ptr<GridModule>> modules,
               std::vector<GridCable>                   cables,
               std::vector<int>                         order);

    GridEngine(GridEngine&&) noexcept            = default;
    GridEngine& operator=(GridEngine&&) noexcept = default;

    GridEngine(const GridEngine&)            = delete;
    GridEngine& operator=(const GridEngine&) = delete;

    // Propagate a sample rate change to all modules and reset the frame counter.
    // Rebuilds TapSchema and PortSchema and increments the epoch counter.
    // Must be called before the first step_block() and on any sample rate change.
    void prepare(float sample_rate);

    // Process n_frames samples.
    // For each sample: call process() on every module in topological order,
    // then route output voltages to input voltages along all cables.
    // Harvests performance tap values into tap_frame() after the last sample.
    void step_block(int n_frames);

    // Number of modules in this engine.
    int module_count() const noexcept { return static_cast<int>(modules_.size()); }

    // Non-owning access to a module by its index (as returned by GridGraph::add_module()).
    // Valid for the lifetime of this GridEngine. Returns nullptr if idx is out of range.
    GridModule* module(int idx) noexcept;
    const GridModule* module(int idx) const noexcept;

    // Schema discovery — stable until the next prepare() call.
    const TapSchema&  tap_schema()  const noexcept { return tap_schema_; }
    const PortSchema& port_schema() const noexcept { return port_schema_; }

    // Write named param-bus inputs before calling step_block().
    // frame[i] maps to port_schema().entries[i]; extra entries are ignored,
    // missing entries leave the corresponding input port unchanged.
    void apply_params(std::span<const float> frame);

    // Read performance tap values after calling step_block().
    // tap_frame()[i] corresponds to tap_schema().entries[i].
    // Valid until the next step_block() or prepare() call.
    std::span<const float> tap_frame() const noexcept;

  private:
    void step_sample();
    void route_cables();
    void build_schemas();
    void harvest_taps();

    std::vector<std::unique_ptr<GridModule>> modules_;
    std::vector<GridCable>                   cables_;
    std::vector<int>                         order_;
    GridProcessArgs                          args_{};
    TapSchema                                tap_schema_;
    PortSchema                               port_schema_;
    std::vector<float>                       tap_frame_;
    uint32_t                                 epoch_{0};
};

} // namespace kairos_grid
