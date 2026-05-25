// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace kairos_grid {

// A single audio-rate port — one floating-point voltage per sample.
//
// Voltage conventions:
//   Audio content: [-1, 1] normalised full-scale.
//   CV content:    [0, 1] unipolar or [-1, 1] bipolar; semantic, not enforced.
//
// v1: monophonic. The field is kept in a struct to allow future polyphony
// without changing the GridModule interface.
struct GridPort {
    float voltage = 0.f;
};

// Per-sample timing context passed to every GridModule::process() call.
struct GridProcessArgs {
    float   sample_rate;   // current sample rate in Hz
    float   sample_time;   // 1.f / sample_rate, in seconds
    int64_t frame;         // audio samples since GridEngine::prepare() was called
};

// Abstract base class for all modules in the kairos-grid sample-rate engine.
//
// A GridModule reads from its `inputs` ports and writes to its `outputs` ports
// once per sample. The engine calls process() in topological order, then routes
// voltages from upstream outputs to downstream inputs via cables — identical to
// VCVRack's Engine_stepFrame() + Engine_stepFrameCables() pattern.
//
// Subclass for:
//   - Faust-generated code  (alembic :kairos-grid compile target)
//   - MI DSP wrappers       (Plaits, Ripples/stmlib Svf, Clouds, ...)
//   - SST DSP wrappers      (FM2/FM3, sst-filters, sst-effects, ...)
//   - Utility modules       (const source, audio I/O bridge, param smoother)
//
// Block-based MI DSP (Plaits kBlockSize=12, Clouds):
//   These objects have a hardcoded 48 kHz sample rate and render in blocks.
//   Their GridModule wrappers maintain an internal frame buffer and call
//   Render() / Process() every kBlockSize samples. The engine is unaware of
//   this; it always calls process() once per sample. See design doc §Internal
//   Engine Model Options for the canonical PlaitsModule pattern.
//
// Feedback loops:
//   The engine graph must be a DAG. Feedback must be encapsulated within a
//   single GridModule (e.g. using the alembic (faust ...) escape for ~ _).
class GridModule {
  public:
    explicit GridModule(int num_inputs, int num_outputs)
        : inputs(static_cast<std::size_t>(num_inputs)),
          outputs(static_cast<std::size_t>(num_outputs)) {}

    virtual ~GridModule() = default;

    // Non-copyable; modules own DSP state.
    GridModule(const GridModule&)            = delete;
    GridModule& operator=(const GridModule&) = delete;

    // Called once at init and whenever sample rate changes, before process().
    // Override to rebuild delay lines, recompute filter coefficients, etc.
    // The default implementation does nothing.
    virtual void prepare(const GridProcessArgs& args) { (void)args; }

    // Called once per sample in topological order.
    // Precondition:  inputs[] contain voltages routed from upstream modules
    //                (or 0.f for unconnected ports).
    // Postcondition: outputs[] contain voltages to be routed downstream.
    virtual void process(const GridProcessArgs& args) = 0;

    std::vector<GridPort> inputs;
    std::vector<GridPort> outputs;
};

} // namespace kairos_grid
