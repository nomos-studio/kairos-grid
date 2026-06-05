// Minimal kairos-grid test fixture.
// Stereo phasor (sawtooth) oscillator with 3 params — exercises JSON sidecar
// parsing, setParamValue dispatch, and the two-output scratch memory layout.
//
// Uses a recursive phasor (no sinf import) so the WASM module is fully
// self-contained and instantiates without any host-provided imports.
//
// Port layout after WasmGridModule::create():
//   inputs[0]  — amp   param CV  (named "amp",    wasm_addr from JSON)
//   inputs[1]  — detune param CV (named "detune", wasm_addr from JSON)
//   inputs[2]  — freq  param CV  (named "freq",   wasm_addr from JSON)
//   outputs[0] — left  audio
//   outputs[1] — right audio

import("stdfaust.lib");

freq   = hslider("freq",   440.0, 20.0, 20000.0, 0.1);
amp    = hslider("amp",    0.5,   0.0,  1.0,     0.001);
detune = hslider("detune", 0.005, 0.0,  0.5,     0.0001);

// Phasor: accumulates 0..1 and wraps; no sinf needed.
phasor(f) = f/ma.SR : (+ : ma.decimal) ~ _;

process = phasor(freq) * amp, phasor(freq * (1.0 + detune)) * amp;
