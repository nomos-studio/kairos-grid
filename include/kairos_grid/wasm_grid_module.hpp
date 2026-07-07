// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <kairos_grid/grid_module.hpp>

#include <memory>
#include <string>
#include <vector>

namespace kairos_grid {

// WasmGridModule — wraps a Faust-compiled .wasm file as a GridModule.
//
// Port layout (all counts determined at construction from the WASM ABI):
//   inputs[0 .. n_audio_in - 1]              — Faust audio inputs
//   inputs[n_audio_in .. n_audio_in + P - 1] — Faust named param inputs (control-rate CV)
//   outputs[0 .. n_audio_out - 1]            — Faust audio outputs
//
// param_ports[] maps each param's label to its input port index, enabling
// nomos-rt's named param bus to address them by string.  Params are named
// "<stem>/<label>" where <stem> is the .wasm filename without extension.
// That prefix is applied externally in the module registry setup function
// once the filename is known; the module stores raw labels until then.
//
// Param values are passed to the WASM setParamValue() export.  Dirty
// tracking avoids redundant WASM calls: a call is only issued when the
// input port voltage changes from the previously applied value.
//
// Audio processing uses compute(dsp=0, count=1, in_ptrs, out_ptrs) called
// once per sample.  This is correct and simple; a block-buffer optimisation
// is deferred (see design note §WasmGridModule performance).
//
// Usage in an EDN patch descriptor:
//   {:modules [{:type "wasm" :wasm-path "/path/to/synth.wasm"}
//              {:type "audio-out"}]
//    :cables  [[0 0 1 0] [0 1 1 1]]}
//
// Requires KAIROS_GRID_PLUGIN_HAS_WASM=1.

struct WasmDspImpl; // fully defined in wasm_grid_module.cpp

class WasmGridModule : public GridModule {
  public:
    // Factory.  Loads and JIT-compiles the .wasm file (cached by path), creates
    // a wasmtime store+instance, queries port counts, and reads the companion
    // .json sidecar for param metadata.
    // Returns nullptr if the file is missing, unreadable, or the WASM is invalid.
    static std::unique_ptr<WasmGridModule> create(const std::string& wasm_path);

    ~WasmGridModule() override;

    // Calls the Faust init(sample_rate) export and allocates scratch WASM memory
    // for the per-sample I/O buffers.  Safe to call more than once (on SR change).
    void prepare(const GridProcessArgs& args) override;

    // Routes audio inputs[0..n_audio_in-1] into WASM compute(0, 1, ...).
    // Applies dirty-tracked param updates via setParamValue.
    // Reads audio outputs back from WASM memory into outputs[].
    void process(const GridProcessArgs& args) override;

    // Number of Faust audio inputs (excludes param ports).
    int n_audio_in() const noexcept { return n_audio_in_; }
    // Number of Faust audio outputs.
    int n_audio_out() const noexcept { return n_audio_out_; }
    // Number of named parameter ports.
    int n_params() const noexcept { return n_params_; }

  private:
    WasmGridModule(std::unique_ptr<WasmDspImpl> impl, int n_audio_in, int n_audio_out,
                   int n_params);

    std::unique_ptr<WasmDspImpl> impl_;
    int                          n_audio_in_{0};
    int                          n_audio_out_{0};
    int                          n_params_{0};

    // Shadow values: last voltage written to each param port.
    // Indexed [0 .. n_params-1].  Initialised to NaN so the first process()
    // call always pushes the initial value to the WASM module.
    std::vector<float> last_param_voltages_;
};

} // namespace kairos_grid
