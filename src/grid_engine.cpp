// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/grid_engine.hpp>

#include <cassert>

namespace kairos_grid {

GridEngine::GridEngine(std::vector<std::unique_ptr<GridModule>> modules,
                       std::vector<GridCable>                   cables,
                       std::vector<int>                         order)
    : modules_(std::move(modules)),
      cables_(std::move(cables)),
      order_(std::move(order)) {}

void GridEngine::prepare(float sample_rate) {
    args_.sample_rate = sample_rate;
    args_.sample_time = 1.f / sample_rate;
    args_.frame       = 0;
    for (auto& m : modules_) {
        m->prepare(args_);
    }
}

void GridEngine::step_block(int n_frames) {
    for (int i = 0; i < n_frames; ++i) {
        step_sample();
        route_cables();
        ++args_.frame;
    }
}

GridModule* GridEngine::module(int idx) noexcept {
    if (idx < 0 || idx >= static_cast<int>(modules_.size())) return nullptr;
    return modules_[static_cast<std::size_t>(idx)].get();
}

const GridModule* GridEngine::module(int idx) const noexcept {
    if (idx < 0 || idx >= static_cast<int>(modules_.size())) return nullptr;
    return modules_[static_cast<std::size_t>(idx)].get();
}

void GridEngine::step_sample() {
    for (int idx : order_) {
        modules_[static_cast<std::size_t>(idx)]->process(args_);
    }
}

void GridEngine::route_cables() {
    for (const auto& c : cables_) {
        modules_[static_cast<std::size_t>(c.to_module)]
            ->inputs[static_cast<std::size_t>(c.to_port)]
            .voltage =
            modules_[static_cast<std::size_t>(c.from_module)]
                ->outputs[static_cast<std::size_t>(c.from_port)]
                .voltage;
    }
}

} // namespace kairos_grid
