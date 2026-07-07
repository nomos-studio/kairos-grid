// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <kairos_grid/grid_engine.hpp>
#include <kairos_grid/result.hpp>

#include <memory>
#include <string>
#include <vector>

namespace kairos_grid {

enum class graph_error {
    cycle_detected,       // graph is not a DAG; feedback must be inside a GridModule
    invalid_module_index, // cable references a module index out of range
    invalid_port_index,   // cable references a port index out of range
};

// Builder for a GridEngine.
//
// Add modules (transferring ownership) and cables, then call build() to
// compute the topological order and produce a ready-to-use GridEngine.
//
// Feedback loops are not supported at the graph level. They must be
// encapsulated within a single GridModule — for alembic patches, this means
// using the (faust ...) escape for ~ _ feedback expressions.
//
// After build() is called, this GridGraph is consumed: the module ownership
// is transferred to the GridEngine, and the graph instance should not be used
// further.
class GridGraph {
  public:
    // Takes ownership of the module.
    // Returns the module index (stable for the lifetime of the built GridEngine).
    int add_module(std::unique_ptr<GridModule> module);

    // Add a directed cable from `from_module`'s output port `from_port` to
    // `to_module`'s input port `to_port`.
    void add_cable(GridCable cable);

    // Validate the graph and compute topological order.
    // On success, returns a GridEngine that owns all added modules.
    // On failure, returns a graph_error describing the problem.
    result<GridEngine, graph_error> build();

  private:
    std::vector<std::unique_ptr<GridModule>> modules_;
    std::vector<GridCable>                   cables_;
};

} // namespace kairos_grid
