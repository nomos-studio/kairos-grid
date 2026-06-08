# Changelog

All notable changes to kairos-grid are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## [Unreleased]

---

## [0.1.0] — 2026-06-07

### Added

#### Core engine

- **`GridModule`** — abstract base for all audio-rate modules; one
  `inputs[]` / `outputs[]` `GridPort` per sample, plus `taps[]`
  (block-rate captures) and `param_ports[]` (named param-bus inputs).
- **`GridGraph` / `GridEngine`** — directed module graph with topological
  sort, cable routing, `step_block()`, `apply_params()`, `tap_frame()`.
- **`EnvironmentModule`** — bridges nomos-rt environment signals (tempo,
  beat/bar phase, is_playing, voice note/gate/velocity) into the grid as
  param-bus values.
- **`AudioInputModule` / `AudioOutputModule`** — stereo I/O bridge between
  CLAP host audio buffers and the grid.

#### CLAP plugin wrapper

- **`clap_plugin.cpp`** — CLAP entry point; wraps GridEngine behind the
  full CLAP ABI (audio ports, params, state, tap/param/patch/hot-swap bus).
- **Patch-bus EDN parser** — zero-dependency recursive-descent parser for
  `{:modules [...] :cables [...]}` descriptors; drives `build_patch_slot`
  and atomic engine swap at block boundary via `pending_slot_`.
- **Module registry** — maps type strings to factory lambdas; built-in:
  `"env"`, `"audio-in"`, `"audio-out"`, `"plaits"` (opt), `"svf"` (opt),
  `"wasm"` (opt).
- **State extension** — binary serialisation of all named param values
  (KGST magic, v1 format; round-trips cleanly across patch swaps).

#### Custom CLAP extensions (plugin-side)

- **`kairos/tap-bus`** — exposes block-rate tap signals to the host for
  modulation routing and scope display.
- **`kairos/param-bus`** — receives named float parameters from nomos-rt
  without CLAP automation overhead; schema versioned via epoch counter.
- **`kairos/patch-bus`** — accepts EDN patch descriptors pushed by kairos;
  atomic engine swap at next `process()` block boundary.
- **`kairos/hot-swap/2`** (`CLAP_EXT_KAIROS_HOT_SWAP`) — gapless WASM
  module replacement. `request(new_path, old_path)`: `old_path` identifies
  the target slot by its current `.wasm` path (NULL = first slot, for
  backward-compatible single-slot patches).

#### Optional DSP modules

- **MI DSP wrappers** (`KAIROS_GRID_BUILD_MI=ON`) — `PlaitsModule` (macro-
  oscillator, 7 param ports), `SvfModule` (state-variable filter, 2 param
  ports), `OnePoleModule`.  Backed by the eurorack library (GPL-3.0).
- **Surge XT wrappers** (`KAIROS_GRID_BUILD_SURGE=ON`) — 8 oscillator
  modules, 4 filter modules, 4 effect modules (reverb, delay, chorus,
  distortion), 4 modulator modules.
- **`WasmGridModule`** (`KAIROS_GRID_BUILD_WASM=ON`) — runs Faust-compiled
  `.wasm` files as `GridModule` instances via wasmtime.  JSON sidecar
  parsed for param metadata (label + WASM memory address).  Scratch memory
  grown once per `prepare()` call.  Dirty-tracked `setParamValue` (NaN
  sentinel ensures first call always pushes all params).  Linker pre-
  populated with 19 Faust math imports (`env._sinf` etc.) so any Faust
  2.80+ patch instantiates without modification.

#### Build system

- **CMakePresets.json** — `dev`, `ci`, `release`, `plugin`, `mi`,
  `ci-mi`, `surge`, `ci-surge`, `plugin-mi`, `wasm`, `ci-wasm`,
  `plugin-wasm` presets for all feature combinations.
- nomos-topology dependency (header-only C++ constants + Clojure schema).

#### Test suite

- Catch2 v3.7.1; 65 tests across engine, WASM module, and plugin extension
  suites.
- **Vendored test fixture** — `tests/fixtures/sine_stereo.{dsp,wasm,json}`:
  a Faust phasor oscillator with 3 hslider params (amp, detune, freq),
  self-contained (no host math imports needed).
- Integration test covers `WasmGridModule` full path: `create()` → port
  layout → `prepare()` (double-call SR change) → `process()` non-zero
  output → detune divergence.

### Fixed

- CLAP SDK `GIT_SHALLOW TRUE` removed; `CLAP_SRC_DIR` override documented.
- `CvChannelDecoderModule` removed — NLC modulator work belongs in nomos-rt.
- Surge XT preset disables plugin build (clap-juce-extensions collision).

### Changed

- nomos-rt dependency pinned to `0d7ef76` for reproducible builds.
