// SPDX-License-Identifier: GPL-3.0-or-later
// buffer — shared named storage; the third part of the CV-addressable buffer archetype.
//
// A BufferModule owns two shared_ptr<vector<float>> buffers (L and R) and provides
// no-signal-path processing.  Its role is allocation: it creates the heap storage that
// peek and poke modules can reference via the same shared_ptr when they share a :buf-id
// in the EDN patch.
//
// Without :buf-id (standalone mode): each BufferModule owns its own L and R buffers.
// With :buf-id (shared mode): the factory pre-creates shared_ptr<vector<float>> pairs
// keyed by buf_id, then passes them to BufferModule, PeekModule, and PokeModule so all
// three reference the same heap memory.
//
// Standalone usage (tests, direct C++):
//   BufferModule b(2048);
//   PeekModule::fill_sine(b.buffer_l());    // fill via ref accessor
//   PeekModule p(PeekInterp::Linear, b.buffer_l_ptr());  // share with peek
//
// Shared-storage composition (EDN patch via :buf-id):
//   {:type "buffer" :buf-id "wt" :size 2048}  — allocates shared storage
//   {:type "peek"   :buf-id "wt"}              — reads from it
//   {:type "poke"   :buf-id "wt"}              — writes to it
//
// process() is a no-op: BufferModule is a storage node, not a signal-path node.
// It participates in the module graph only to materialise the shared_ptrs; signal
// evaluation passes through it with nothing to do.
//
// Ports: 0 inputs, 0 outputs.

#pragma once

#include <kairos_grid/grid_module.hpp>

#include <memory>
#include <vector>

namespace kairos_grid {

class BufferModule : public GridModule {
  public:
    // Standalone: allocates its own buffers.
    explicit BufferModule(std::size_t size = 2048)
        : GridModule(0, 0), buf_l_(std::make_shared<std::vector<float>>(size, 0.f)),
          buf_r_(std::make_shared<std::vector<float>>(size, 0.f)) {}

    // Shared: receives pre-created buffers from the factory's buf_id registry.
    BufferModule(std::shared_ptr<std::vector<float>> l, std::shared_ptr<std::vector<float>> r)
        : GridModule(0, 0), buf_l_(std::move(l)), buf_r_(std::move(r)) {}

    // shared_ptr accessors — hand to PeekModule / PokeModule constructors.
    std::shared_ptr<std::vector<float>> buffer_l_ptr() const noexcept { return buf_l_; }
    std::shared_ptr<std::vector<float>> buffer_r_ptr() const noexcept { return buf_r_; }

    // Reference accessors — fill helpers and tests work on vector<float>& directly.
    std::vector<float>&       buffer_l() noexcept { return *buf_l_; }
    const std::vector<float>& buffer_l() const noexcept { return *buf_l_; }

    std::vector<float>&       buffer_r() noexcept { return *buf_r_; }
    const std::vector<float>& buffer_r() const noexcept { return *buf_r_; }

    void process(const GridProcessArgs&) override {}

  private:
    std::shared_ptr<std::vector<float>> buf_l_;
    std::shared_ptr<std::vector<float>> buf_r_;
};

} // namespace kairos_grid
