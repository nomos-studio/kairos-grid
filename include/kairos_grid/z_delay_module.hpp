// SPDX-License-Identifier: GPL-3.0-or-later
// Single-sample unit delay — the z⁻¹ operator in Z-transform notation.
//
// The only mechanism through which a feedback path may be constructed in the
// kairos-grid DAG.  All other modules are pure feedforward; every feedback arc
// must pass through a ZDelayModule.
//
// Input:  0  in   — signal to delay
// Output: 0  out  — signal delayed by exactly one sample
//
// No parameters.  The delay is unconditional and always exactly one sample.
// prepare() resets the internal register to 0.0.
//
// Registration:
//   "z-1"   — user-visible name (Z-transform convention)
//   "_z-1"  — compiler-generated name (Alembic inserts this automatically to
//             break detected feedback arcs; leading underscore marks it as
//             synthetic so tooling can distinguish it from intentional uses)
#pragma once

#include <kairos_grid/grid_module.hpp>

namespace kairos_grid {

class ZDelayModule : public GridModule {
  public:
    ZDelayModule() : GridModule(1, 1) {}

    void prepare(const GridProcessArgs&) override { prev_ = 0.f; }

    void process(const GridProcessArgs&) override {
        outputs[0].voltage = prev_;
        prev_              = inputs[0].voltage;
    }

  private:
    float prev_{0.f};
};

} // namespace kairos_grid
