# kairos-grid

Sample-rate modular DSP engine packaged as a CLAP plugin.  Part of the
[nomos-studio](https://github.com/nomos-studio) platform; hosted by
[kairos](https://github.com/nomos-studio/kairos) and controlled by
[nous](https://github.com/nomos-studio/nous) via the patch-bus extension.

Conceptually similar to Bitwig Grid: a directed graph of audio-rate modules
wired by cables, reconfigurable at runtime without restarting the host.

## Architecture

```
nous (Clojure) → kairos (CLAP host) → kairos-grid (this plugin)
                          ↕ patch-bus EDN descriptor
                    GridEngine
                    ├── EnvironmentModule   (tempo, beat, transport, voice)
                    ├── AudioInputModule    (host audio in → grid)
                    ├── AudioOutputModule   (grid → host audio out)
                    ├── PlaitsModule        (MI macro-oscillator, optional)
                    ├── SvfModule           (MI state-variable filter, optional)
                    ├── Surge XT modules    (oscillators, filters, effects, optional)
                    └── WasmGridModule      (Faust-compiled .wasm, optional)
```

## Build presets

```bash
# Engine only (no CLAP, no optional DSP)
cmake --preset dev && cmake --build --preset dev

# CLAP plugin + tests
cmake --preset plugin && cmake --build --preset plugin

# With Faust WASM module support (downloads wasmtime pre-built binaries)
cmake --preset wasm && cmake --build --preset wasm

# Full plugin + WASM
cmake --preset plugin-wasm && cmake --build --preset plugin-wasm
```

Run tests:

```bash
ctest --preset dev          # engine tests
ctest --preset wasm         # includes WasmGridModule integration tests
ctest --preset plugin-wasm  # full suite including hot-swap extension
```

**Requirements:** cmake ≥ 3.20, C++20, Ninja.
Optional: eurorack sources (MI), Surge XT sources, wasmtime (auto-downloaded).

## Patch-bus EDN format

```edn
{:modules [{:type "env"}
           {:type "plaits"   :params {:harmonics 0.5 :timbre 0.5}}
           {:type "svf"      :params {:cutoff 0.4 :q 0.2}}
           {:type "audio-out"}]
 :cables  [[0 0 1 0]   ; env voice_note  → plaits note
           [0 1 1 4]   ; env voice_gate  → plaits gate
           [1 0 2 0]   ; plaits out      → svf in
           [2 0 3 0]   ; svf out         → audio-out left
           [2 0 3 1]]} ; svf out         → audio-out right
```

Module types: `"env"`, `"audio-in"`, `"audio-out"`, `"plaits"` (opt),
`"svf"` (opt), `"wasm"` (opt — requires `:wasm-path`).

## Faust WASM modules

Load any Faust-compiled `.wasm` file as a grid module:

```edn
{:modules [{:type "wasm" :wasm-path "/tmp/my-synth.wasm"}
           {:type "audio-out"}]
 :cables  [[0 0 1 0] [0 1 1 1]]}
```

Hot-swap (block-boundary, no audio gap) via the kairos/hot-swap extension:

```clojure
;; Single-slot patch
(kairos/send-wasm-hot-swap! :voice "/tmp/new-synth.wasm")

;; Multi-slot patch — identify slot by its current path
(kairos/send-wasm-hot-swap! :voice "/tmp/new-filter.wasm"
                             :replace-path "/tmp/old-filter.wasm")
```

## Custom CLAP extensions

kairos-grid exposes four proprietary extensions alongside the standard CLAP set:

| Extension ID | Purpose |
|---|---|
| `kairos/tap-bus` | Read-only block-rate signal taps (scope, analysis) |
| `kairos/param-bus` | Named float parameters without CLAP automation overhead |
| `kairos/patch-bus` | Push EDN graph descriptor; atomic engine swap at block boundary |
| `kairos/hot-swap/2` | Gapless WASM DSP module replacement |

Headers in `include/kairos_grid/`.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).
