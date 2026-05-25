// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <kairos_grid/grid_module.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace kairos_grid {

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
    // Must be called before the first step_block() and on any sample rate change.
    void prepare(float sample_rate);

    // Process n_frames samples.
    // For each sample: call process() on every module in topological order,
    // then route output voltages to input voltages along all cables.
    void step_block(int n_frames);

    // Number of modules in this engine.
    int module_count() const noexcept { return static_cast<int>(modules_.size()); }

    // Non-owning access to a module by its index (as returned by GridGraph::add_module()).
    // Valid for the lifetime of this GridEngine. Returns nullptr if idx is out of range.
    GridModule* module(int idx) noexcept;
    const GridModule* module(int idx) const noexcept;

  private:
    void step_sample();
    void route_cables();

    std::vector<std::unique_ptr<GridModule>> modules_;
    std::vector<GridCable>                   cables_;
    std::vector<int>                         order_;
    GridProcessArgs                          args_{};
};

} // namespace kairos_grid
