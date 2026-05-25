// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/grid_graph.hpp>

#include <algorithm>
#include <queue>
#include <vector>

namespace kairos_grid {

int GridGraph::add_module(std::unique_ptr<GridModule> module) {
    int idx = static_cast<int>(modules_.size());
    modules_.push_back(std::move(module));
    return idx;
}

void GridGraph::add_cable(GridCable cable) {
    cables_.push_back(cable);
}

result<GridEngine, graph_error> GridGraph::build() {
    const int n = static_cast<int>(modules_.size());

    // Validate cable port indices before attempting the sort.
    for (const auto& c : cables_) {
        if (c.from_module < 0 || c.from_module >= n ||
            c.to_module   < 0 || c.to_module   >= n)
            return unexpected<graph_error>{graph_error::invalid_module_index};

        const auto& from_out = modules_[static_cast<std::size_t>(c.from_module)]->outputs;
        const auto& to_in    = modules_[static_cast<std::size_t>(c.to_module)]->inputs;

        if (c.from_port < 0 || c.from_port >= static_cast<int>(from_out.size()) ||
            c.to_port   < 0 || c.to_port   >= static_cast<int>(to_in.size()))
            return unexpected<graph_error>{graph_error::invalid_port_index};
    }

    // Kahn's algorithm — topological sort with cycle detection.
    std::vector<int>              in_degree(static_cast<std::size_t>(n), 0);
    std::vector<std::vector<int>> adj(static_cast<std::size_t>(n));

    for (const auto& c : cables_) {
        adj[static_cast<std::size_t>(c.from_module)].push_back(c.to_module);
        ++in_degree[static_cast<std::size_t>(c.to_module)];
    }

    std::queue<int> q;
    for (int i = 0; i < n; ++i) {
        if (in_degree[static_cast<std::size_t>(i)] == 0) q.push(i);
    }

    std::vector<int> order;
    order.reserve(static_cast<std::size_t>(n));

    while (!q.empty()) {
        int u = q.front();
        q.pop();
        order.push_back(u);
        for (int v : adj[static_cast<std::size_t>(u)]) {
            if (--in_degree[static_cast<std::size_t>(v)] == 0) q.push(v);
        }
    }

    if (static_cast<int>(order.size()) != n)
        return unexpected<graph_error>{graph_error::cycle_detected};

    // Transfer module ownership to the engine.
    return GridEngine{std::move(modules_), cables_, std::move(order)};
}

} // namespace kairos_grid
