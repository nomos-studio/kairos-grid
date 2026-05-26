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
    build_schemas();
}

void GridEngine::step_block(int n_frames) {
    for (int i = 0; i < n_frames; ++i) {
        step_sample();
        route_cables();
        ++args_.frame;
    }
    harvest_taps();
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

void GridEngine::build_schemas() {
    ++epoch_;

    tap_schema_.entries.clear();
    port_schema_.entries.clear();
    tap_schema_.epoch  = epoch_;
    port_schema_.epoch = epoch_;

    for (int mi = 0; mi < static_cast<int>(modules_.size()); ++mi) {
        const auto* m = modules_[static_cast<std::size_t>(mi)].get();

        for (int ti = 0; ti < static_cast<int>(m->taps.size()); ++ti) {
            int id = static_cast<int>(tap_schema_.entries.size());
            tap_schema_.entries.push_back({id, m->taps[static_cast<std::size_t>(ti)].name, mi, ti});
        }

        for (int pi = 0; pi < static_cast<int>(m->param_ports.size()); ++pi) {
            const auto& pp = m->param_ports[static_cast<std::size_t>(pi)];
            int id = static_cast<int>(port_schema_.entries.size());
            port_schema_.entries.push_back({id, pp.name, mi, pp.port_idx});
        }
    }

    tap_frame_.assign(tap_schema_.entries.size(), 0.f);
}

void GridEngine::harvest_taps() {
    for (const auto& e : tap_schema_.entries) {
        tap_frame_[static_cast<std::size_t>(e.id)] =
            modules_[static_cast<std::size_t>(e.module_idx)]
                ->taps[static_cast<std::size_t>(e.tap_idx)].value;
    }
}

void GridEngine::apply_params(std::span<const float> frame) {
    const auto& entries = port_schema_.entries;
    const int n = std::min(static_cast<int>(frame.size()),
                           static_cast<int>(entries.size()));
    for (int i = 0; i < n; ++i) {
        const auto& e = entries[static_cast<std::size_t>(i)];
        modules_[static_cast<std::size_t>(e.module_idx)]
            ->inputs[static_cast<std::size_t>(e.port_idx)].voltage = frame[i];
    }
}

std::span<const float> GridEngine::tap_frame() const noexcept {
    return tap_frame_;
}

} // namespace kairos_grid
