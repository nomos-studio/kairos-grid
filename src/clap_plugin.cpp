// SPDX-License-Identifier: GPL-3.0-or-later
// CLAP entry point for kairos-grid — wraps GridEngine behind the CLAP ABI.
//
// Graph: EnvironmentModule + AudioInputModule + AudioOutputModule.
//   - AudioInput/Output bridge CLAP audio buffers to per-sample grid I/O.
//   - Param bus (nomos-rt → grid) exposed as CLAP automatable parameters.
//   - Transport and note events drive EnvironmentModule via the param frame.
//   - patch-bus extension accepts EDN graph descriptors and swaps the engine
//     atomically at the next process() block boundary.

#include <kairos_grid/audio_modules.hpp>
#include <kairos_grid/buffer/buffer_module.hpp>
#include <kairos_grid/buffer/peek_module.hpp>
#include <kairos_grid/buffer/poke_module.hpp>
#include <kairos_grid/buffer/sah_module.hpp>
#include <kairos_grid/clap_kairos_param_bus.h>
#include <kairos_grid/clap_kairos_patch_bus.h>
#include <kairos_grid/clap_kairos_tap_bus.h>
#include <kairos_grid/clock/clock_division_module.hpp>
#include <kairos_grid/environment_module.hpp>
#include <kairos_grid/grid_graph.hpp>
#include <kairos_grid/shaper/shaper_module.hpp>
#include <kairos_grid/z_delay_module.hpp>

#if defined(KAIROS_GRID_PLUGIN_HAS_AIRWINDOWS)
#include <kairos_grid/airwindows/airwindows_module.hpp>
#endif

#if defined(KAIROS_GRID_PLUGIN_HAS_WASM)
#include <kairos_grid/clap_kairos_hot_swap.h>
#include <kairos_grid/wasm_grid_module.hpp>
#endif

#if defined(KAIROS_GRID_PLUGIN_HAS_FFT)
#include <kairos_grid/fft/bin_shift_module.hpp>
#include <kairos_grid/fft/fft_module.hpp>
#include <kairos_grid/fft/partial_tracker_module.hpp>
#include <kairos_grid/fft/spectral_freeze_module.hpp>
#include <kairos_grid/fft/spectral_gate_module.hpp>
#include <kairos_grid/fft/spectral_peaks_module.hpp>
#include <kairos_grid/fft/spectral_smear_module.hpp>
#endif

#if defined(KAIROS_GRID_PLUGIN_HAS_WDF)
#include <kairos_grid/wdf/wdf_modules.hpp>
#endif

#include <kairos_grid/clap_kairos_vcv_ctrl.h>
#include <kairos_grid/grid_extension.hpp>
#include <kairos_grid/vcv_bridge/vcv_bridge_module.hpp>

#include <clap/clap.h>

#include <dlfcn.h>
#include <filesystem>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kairos_grid {

// ---------------------------------------------------------------------------
// Plugin descriptor
// ---------------------------------------------------------------------------

static const char* k_features[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    nullptr,
};

static const clap_plugin_descriptor_t k_descriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id           = "studio.nomos.kairos-grid",
    .name         = "kairos-grid",
    .vendor       = "nomos-studio",
    .url          = "",
    .manual_url   = "",
    .support_url  = "",
    .version      = "0.1.0",
    .description  = "Bitwig Grid-style modular engine with nomos-rt control plane",
    .features     = k_features,
};

// ---------------------------------------------------------------------------
// Module registry
//
// Maps a type name string to a factory + optional post-construction setup
// function.  The setup function receives the raw GridModule pointer and the
// param-port name prefix (module type for v1).
//
// All built-in types are registered at first call via Meyers singleton.
// ---------------------------------------------------------------------------

// Concrete registry — holds built-in module types and accepts add() calls
// from .kgext extensions loaded at startup.
class ModuleRegistryImpl : public GridModuleRegistry {
  public:
    void add(std::string key, ModuleSpec spec) override {
        reg_.emplace(std::move(key), std::move(spec));
    }
    const std::unordered_map<std::string, ModuleSpec>& map() const { return reg_; }

  private:
    std::unordered_map<std::string, ModuleSpec> reg_;
};

static ModuleRegistryImpl& get_module_registry() {
    static ModuleRegistryImpl registry;
    return registry;
}

static void populate_builtins(ModuleRegistryImpl& r) {
    r.add("env",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<EnvironmentModule>(); },
           nullptr, nullptr, nullptr});
    r.add("audio-in",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<AudioInputModule>(); },
           nullptr, nullptr, nullptr});
    r.add("audio-out",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<AudioOutputModule>(); },
           nullptr, nullptr, nullptr});
    // Shaper primitive — memoryless nonlinearity with CV-modulatable drive, shape, mix.
    // Three patch-time curves: tanh (soft→hard saturation), fold (sine wavefold),
    // quantize (bitcrush).  Tanh and Fold use ADAA; Quantize is intentionally raw.
    // Inputs: in-l(0), in-r(1), drive(2), shape(3), mix(4).
    {
        auto shaper_setup = [](GridModule* m, const std::string& pfx) {
            m->param_ports = {
                {pfx + "/drive", 2},
                {pfx + "/shape", 3},
                {pfx + "/mix", 4},
            };
        };
        r.add("shaper-tanh", {[]() -> std::unique_ptr<GridModule> {
                                  return std::make_unique<ShaperModule>(ShaperCurve::Tanh);
                              },
                              shaper_setup, nullptr, nullptr});
        r.add("shaper-fold", {[]() -> std::unique_ptr<GridModule> {
                                  return std::make_unique<ShaperModule>(ShaperCurve::Fold);
                              },
                              shaper_setup, nullptr, nullptr});
        r.add("shaper-quant", {[]() -> std::unique_ptr<GridModule> {
                                   return std::make_unique<ShaperModule>(ShaperCurve::Quantize);
                               },
                               shaper_setup, nullptr, nullptr});
    }
    // Peek primitive — CV-indexed buffer read (the read half of the CV-addressable
    // buffer archetype).  Index input [0,1] maps to buffer positions [0, N-1].
    // Interpolation kernel is patch-time (registered as distinct types).
    // Inputs: index-l(0), index-r(1).  Outputs: out-l(0), out-r(1).
    // Blank-buffer (zeroes) variants: "peek" (linear), "peek-nn" (nearest),
    //   "peek-cubic" (cubic).
    // Pre-filled variants: "peek-sine", "peek-tri", "peek-saw" (all linear interp).
    r.add("peek", {[]() -> std::unique_ptr<GridModule> {
                       return std::make_unique<PeekModule>(PeekInterp::Linear);
                   },
                   nullptr, nullptr, nullptr});
    r.add("peek-nn", {[]() -> std::unique_ptr<GridModule> {
                          return std::make_unique<PeekModule>(PeekInterp::None);
                      },
                      nullptr, nullptr, nullptr});
    r.add("peek-cubic", {[]() -> std::unique_ptr<GridModule> {
                             return std::make_unique<PeekModule>(PeekInterp::Cubic);
                         },
                         nullptr, nullptr, nullptr});
    r.add("peek-sine", {nullptr, nullptr,
                        [](const std::string&) -> std::unique_ptr<GridModule> {
                            auto m = std::make_unique<PeekModule>(PeekInterp::Linear);
                            PeekModule::fill_sine(m->buffer());
                            return m;
                        },
                        nullptr});
    r.add("peek-tri", {nullptr, nullptr,
                       [](const std::string&) -> std::unique_ptr<GridModule> {
                           auto m = std::make_unique<PeekModule>(PeekInterp::Linear);
                           PeekModule::fill_triangle(m->buffer());
                           return m;
                       },
                       nullptr});
    r.add("peek-saw", {nullptr, nullptr,
                       [](const std::string&) -> std::unique_ptr<GridModule> {
                           auto m = std::make_unique<PeekModule>(PeekInterp::Linear);
                           PeekModule::fill_saw(m->buffer());
                           return m;
                       },
                       nullptr});
    // SAH — sample-and-hold, dual-use: modulation S&H (rising-edge trig) and
    // SR reduction (internal periodic counter via rate CV).  Both trigger sources
    // are active simultaneously; whichever fires first latches.
    // Inputs: in-l(0), in-r(1), trig(2), rate(3).  Outputs: out-l(0), out-r(1).
    {
        auto sah_setup = [](GridModule* m, const std::string& pfx) {
            m->param_ports = {
                {pfx + "/trig", 2},
                {pfx + "/rate", 3},
            };
        };
        r.add("sah", {[]() -> std::unique_ptr<GridModule> { return std::make_unique<SahModule>(); },
                      sah_setup, nullptr, nullptr});
    }
    // Poke primitive — CV-gated buffer write with immediate linear readback.
    // The write half of the CV-addressable buffer archetype; peek is the read-only
    // wavetable oscillator half.  Separate write-pos and read-pos allow independent
    // capture and playback heads.  Write-before-read each block: gated capture shows
    // up in the same block's output.
    // Inputs: in-l(0), in-r(1), write-pos(2), read-pos(3), gate(4).
    // Outputs: out-l(0), out-r(1).
    {
        auto poke_setup = [](GridModule* m, const std::string& pfx) {
            m->param_ports = {
                {pfx + "/write-pos", 2},
                {pfx + "/read-pos", 3},
                {pfx + "/gate", 4},
            };
        };
        r.add("poke",
              {[]() -> std::unique_ptr<GridModule> { return std::make_unique<PokeModule>(); },
               poke_setup, nullptr, nullptr});
    }
    // Buffer primitive — shared named storage, 0 inputs / 0 outputs.
    // Standalone (no :buf-id in EDN): owns its own buffers, useful for pre-fill.
    // With :buf-id: the factory injects pre-created shared_ptrs so peek and poke
    // on the same :buf-id reference the same heap storage.
    r.add("buffer",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<BufferModule>(); },
           nullptr, nullptr, nullptr});
    // Beat-clock subdivision gate — derives from EnvironmentModule beat_phase output.
    // Inputs: beat_phase(0), division(1), pulse_width(2), phase_offset(3).
    // Output: gate 0.0/1.0.  Param ports: clock/division, clock/pulse_width,
    // clock/phase_offset.  Tap: signal/gate.
    // Typical patch: env beat_phase output → clock-div input 0; gate output → trigger
    // target.
    r.add("clock-div",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<ClockDivisionModule>(); },
           nullptr, nullptr, nullptr});
#if defined(KAIROS_GRID_PLUGIN_HAS_AIRWINDOWS)
    {
        // Airwindows saturation modules — no external dependencies.
        // Audio convention: ±1 V normalised full-scale; no internal scaling.
        // aw-desk:   in-l(0), in-r(1) → out-l(0), out-r(1).  No params.
        // aw-slew:   in-l(0), in-r(1), slew(2, [0,1]V) → out-l(0), out-r(1).
        // aw-spiral: in-l(0), in-r(1), drive(2, [0,1]V), wet(3, [0,1]V)
        //            → out-l(0), out-r(1).
        using namespace airwindows;

        r.add("aw-desk",
              {[]() -> std::unique_ptr<GridModule> { return std::make_unique<DeskModule>(); },
               nullptr, nullptr, nullptr});

        r.add("aw-slew",
              {[]() -> std::unique_ptr<GridModule> { return std::make_unique<SlewModule>(); },
               nullptr, nullptr, nullptr});

        r.add("aw-spiral",
              {[]() -> std::unique_ptr<GridModule> { return std::make_unique<SpiralModule>(); },
               nullptr, nullptr, nullptr});
    }
#endif
#if defined(KAIROS_GRID_PLUGIN_HAS_WASM)
    r.add("wasm", {nullptr, // make (unused — wasm uses make_with_args)
                   nullptr, // setup (unused — wasm uses setup_with_args)
                   [](const std::string& wasm_path) -> std::unique_ptr<GridModule> {
                       return WasmGridModule::create(wasm_path);
                   },
                   [](GridModule* m, const std::string& /*type*/, const std::string& stem) {
                       for (auto& pp : m->param_ports)
                           pp.name = stem + "/" + pp.name;
                   }});
#endif
#if defined(KAIROS_GRID_PLUGIN_HAS_FFT)
    // Spectral analysis tap — 2 audio inputs, 6 CV descriptor outputs.
    // Observes audio without altering it; fan-out from same source is free.
    using kairos_grid::FftModule;
    r.add("fft", {[]() -> std::unique_ptr<GridModule> { return std::make_unique<FftModule>(); },
                  nullptr, nullptr, nullptr});
    r.add("fft-512",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<FftModule>(512); },
           nullptr, nullptr, nullptr});
    r.add("fft-2048",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<FftModule>(2048); },
           nullptr, nullptr, nullptr});
    r.add("fft-4096",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<FftModule>(4096); },
           nullptr, nullptr, nullptr});

    // Spectral freeze resynthesis — 2 audio inputs + freeze gate, 2 audio outputs.
    // Latches magnitude spectrum on rising edge; resynthesizes via IFFT + random phases.
    using kairos_grid::SpectralFreezeModule;
    r.add("spectral-freeze",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<SpectralFreezeModule>(); },
           nullptr, nullptr, nullptr});
    r.add("spectral-freeze-512", {[]() -> std::unique_ptr<GridModule> {
                                      return std::make_unique<SpectralFreezeModule>(512);
                                  },
                                  nullptr, nullptr, nullptr});
    r.add("spectral-freeze-2048", {[]() -> std::unique_ptr<GridModule> {
                                       return std::make_unique<SpectralFreezeModule>(2048);
                                   },
                                   nullptr, nullptr, nullptr});
    r.add("spectral-freeze-4096", {[]() -> std::unique_ptr<GridModule> {
                                       return std::make_unique<SpectralFreezeModule>(4096);
                                   },
                                   nullptr, nullptr, nullptr});

    // Spectral smear — Panharmonium-style temporal averaging + density + IFFT resynthesis.
    // 4 inputs (in-l, in-r, smear [0,1], density [0,1]), 2 audio outputs.
    using kairos_grid::SpectralSmearModule;
    r.add("spectral-smear",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<SpectralSmearModule>(); },
           nullptr, nullptr, nullptr});
    r.add("spectral-smear-512", {[]() -> std::unique_ptr<GridModule> {
                                     return std::make_unique<SpectralSmearModule>(512);
                                 },
                                 nullptr, nullptr, nullptr});
    r.add("spectral-smear-2048", {[]() -> std::unique_ptr<GridModule> {
                                      return std::make_unique<SpectralSmearModule>(2048);
                                  },
                                  nullptr, nullptr, nullptr});
    r.add("spectral-smear-4096", {[]() -> std::unique_ptr<GridModule> {
                                      return std::make_unique<SpectralSmearModule>(4096);
                                  },
                                  nullptr, nullptr, nullptr});

    // Spectral gate — per-bin magnitude gating; original-phase synthesis.
    // 4 inputs (in-l, in-r, threshold [0,1], floor [0,1]), 2 audio outputs.
    using kairos_grid::SpectralGateModule;
    r.add("spectral-gate",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<SpectralGateModule>(); },
           nullptr, nullptr, nullptr});
    r.add("spectral-gate-512", {[]() -> std::unique_ptr<GridModule> {
                                    return std::make_unique<SpectralGateModule>(512);
                                },
                                nullptr, nullptr, nullptr});
    r.add("spectral-gate-2048", {[]() -> std::unique_ptr<GridModule> {
                                     return std::make_unique<SpectralGateModule>(2048);
                                 },
                                 nullptr, nullptr, nullptr});
    r.add("spectral-gate-4096", {[]() -> std::unique_ptr<GridModule> {
                                     return std::make_unique<SpectralGateModule>(4096);
                                 },
                                 nullptr, nullptr, nullptr});

    // Bin-shift — frequency-domain pitch shift by integer bin displacement.
    // 3 inputs (in-l, in-r, shift [-1,1]), 2 audio outputs.
    using kairos_grid::BinShiftModule;
    r.add("bin-shift",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<BinShiftModule>(); },
           nullptr, nullptr, nullptr});
    r.add("bin-shift-512",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<BinShiftModule>(512); },
           nullptr, nullptr, nullptr});
    r.add("bin-shift-2048",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<BinShiftModule>(2048); },
           nullptr, nullptr, nullptr});
    r.add("bin-shift-4096",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<BinShiftModule>(4096); },
           nullptr, nullptr, nullptr});

    // Spectral-peaks — STFT peak detector; up to 8 peaks as CV + trigger.
    // 3 inputs (in-l, in-r, threshold [0,1]), 17 outputs (freq×8, amp×8, trigger).
    using kairos_grid::SpectralPeaksModule;
    r.add("spectral-peaks",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<SpectralPeaksModule>(); },
           nullptr, nullptr, nullptr});
    r.add("spectral-peaks-512", {[]() -> std::unique_ptr<GridModule> {
                                     return std::make_unique<SpectralPeaksModule>(512);
                                 },
                                 nullptr, nullptr, nullptr});
    r.add("spectral-peaks-2048", {[]() -> std::unique_ptr<GridModule> {
                                      return std::make_unique<SpectralPeaksModule>(2048);
                                  },
                                  nullptr, nullptr, nullptr});
    r.add("spectral-peaks-4096", {[]() -> std::unique_ptr<GridModule> {
                                      return std::make_unique<SpectralPeaksModule>(4096);
                                  },
                                  nullptr, nullptr, nullptr});

    // Partial-tracker — polyphonic voice allocator for spectral-peaks output.
    // 18 inputs (freq×8, amp×8, trigger, smooth), 16 outputs (voice_freq×8, voice_amp×8).
    using kairos_grid::PartialTrackerModule;
    r.add("partial-tracker",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<PartialTrackerModule>(); },
           nullptr, nullptr, nullptr});
#endif
#if defined(KAIROS_GRID_PLUGIN_HAS_WDF)
    // WDF circuit models — physically accurate nonlinear modules.
    // 2 inputs (audio in, drive), 1 output (audio out).
    using kairos_grid::DiodeClipModule;
    using kairos_grid::DiodeHalfModule;
    r.add("diode-clip",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<DiodeClipModule>(); },
           nullptr, nullptr, nullptr});
    r.add("diode-half",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<DiodeHalfModule>(); },
           nullptr, nullptr, nullptr});
#endif
    // z-1 — single-sample unit delay.  The only mechanism through which a
    // feedback arc may be constructed in the kairos-grid DAG.
    // "z-1"  : user-visible name (Z-transform convention; opt-in feedback)
    // "_z-1" : compiler-generated name (Alembic auto-inserts to break cycles)
    r.add("z-1", {[]() -> std::unique_ptr<GridModule> { return std::make_unique<ZDelayModule>(); },
                  nullptr, nullptr, nullptr});
    r.add("_z-1", {[]() -> std::unique_ptr<GridModule> { return std::make_unique<ZDelayModule>(); },
                   nullptr, nullptr, nullptr});
}

// ---------------------------------------------------------------------------
// Minimal EDN patch descriptor parser (v1)
//
// Supported format:
//   {:modules [{:type "t1"} {:type "t2"} ...]
//    :cables  [[from-mod from-port to-mod to-port] ...]}
//
// Modules are indexed 0-based in the order they appear in :modules.
// Cable integers are module indices (not keyword IDs).  Unknown keys are
// ignored.  This is a forward-compatible subset — full EDN via edn-cpp later.
// ---------------------------------------------------------------------------

struct ParsedModule {
    std::string type;
    std::string wasm_path;
    std::string buf_id;        // :buf-id  — links buffer/peek/poke to shared storage
    int         size{0};       // :size    — buffer size; 0 = use default (2048)
    std::string shm_name;      // :shm-name — POSIX shm name prefix for "vcv-bridge"
    int         n_channels{2}; // :n-channels — audio channel count for "vcv-bridge"
};
struct ParsedCable {
    int from_mod, from_port, to_mod, to_port;
};
struct ParsedPatch {
    std::vector<ParsedModule> modules;
    std::vector<ParsedCable>  cables;
};

namespace {

    static std::size_t find_section_start(const std::string& s, const char* key, char open) {
        const std::size_t kpos = s.find(key);
        if (kpos == std::string::npos)
            return std::string::npos;
        const std::size_t bpos = s.find(open, kpos + std::strlen(key));
        return (bpos == std::string::npos) ? std::string::npos : bpos + 1;
    }

    static std::string extract_str_field(const std::string& s, const char* edn_key,
                                         std::size_t start, std::size_t end) {
        const std::string k = std::string(edn_key) + " \"";
        const std::size_t p = s.find(k, start);
        if (p == std::string::npos || p >= end)
            return {};
        const std::size_t vs = p + k.size();
        const std::size_t ve = s.find('"', vs);
        if (ve == std::string::npos || ve >= end)
            return {};
        return s.substr(vs, ve - vs);
    }

    static std::string extract_type(const std::string& s, std::size_t start, std::size_t end) {
        return extract_str_field(s, ":type", start, end);
    }

    static bool scan_int(const std::string& s, std::size_t& pos, int& out) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\n' || s[pos] == '\r' ||
                                  s[pos] == '\t' || s[pos] == ','))
            ++pos;
        if (pos >= s.size())
            return false;
        bool neg = (s[pos] == '-');
        if (neg)
            ++pos;
        if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos])))
            return false;
        int n = 0;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
            n = n * 10 + (s[pos++] - '0');
        out = neg ? -n : n;
        return true;
    }

    static int extract_int_field(const std::string& s, const char* edn_key, std::size_t start,
                                 std::size_t end) {
        const std::string k = std::string(edn_key) + " ";
        const std::size_t p = s.find(k, start);
        if (p == std::string::npos || p >= end)
            return 0;
        std::size_t vpos = p + k.size();
        if (vpos >= end)
            return 0;
        int val = 0;
        if (!scan_int(s, vpos, val) || vpos > end)
            return 0;
        return val;
    }

} // namespace

static ParsedPatch parse_patch_edn(const char* edn_str, uint32_t len) {
    ParsedPatch result;
    if (!edn_str || len == 0)
        return result;
    const std::string s(edn_str, len);

    // Parse :modules [...]
    const std::size_t mstart = find_section_start(s, ":modules", '[');
    if (mstart != std::string::npos) {
        std::size_t pos   = mstart;
        int         depth = 1;
        while (pos < s.size() && depth > 0) {
            const char c = s[pos];
            if (c == '[') {
                ++depth;
                ++pos;
                continue;
            }
            if (c == ']') {
                if (--depth == 0)
                    break;
                ++pos;
                continue;
            }
            if (c == '{') {
                const std::size_t map_start = pos;
                int               mdepth    = 1;
                ++pos;
                while (pos < s.size() && mdepth > 0) {
                    if (s[pos] == '{')
                        ++mdepth;
                    else if (s[pos] == '}')
                        --mdepth;
                    ++pos;
                }
                const std::string t = extract_type(s, map_start, pos);
                if (!t.empty()) {
                    const std::string wp  = extract_str_field(s, ":wasm-path", map_start, pos);
                    const std::string bi  = extract_str_field(s, ":buf-id", map_start, pos);
                    const int         sz  = extract_int_field(s, ":size", map_start, pos);
                    const std::string sn  = extract_str_field(s, ":shm-name", map_start, pos);
                    int               nch = extract_int_field(s, ":n-channels", map_start, pos);
                    if (nch == 0)
                        nch = 2;
                    result.modules.push_back({t, wp, bi, sz, sn, nch});
                }
            } else {
                ++pos;
            }
        }
    }

    // Parse :cables [...]
    const std::size_t cstart = find_section_start(s, ":cables", '[');
    if (cstart != std::string::npos) {
        std::size_t pos   = cstart;
        int         depth = 1;
        while (pos < s.size() && depth > 0) {
            const char c = s[pos];
            if (c == ']') {
                if (--depth == 0)
                    break;
                ++pos;
                continue;
            }
            if (c == '[') {
                ++depth;
                if (depth == 2) {
                    ++pos; // skip past '['
                    ParsedCable cable{};
                    if (scan_int(s, pos, cable.from_mod) && scan_int(s, pos, cable.from_port) &&
                        scan_int(s, pos, cable.to_mod) && scan_int(s, pos, cable.to_port))
                        result.cables.push_back(cable);
                    while (pos < s.size() && s[pos] != ']')
                        ++pos;
                    if (pos < s.size())
                        ++pos;
                    --depth;
                    continue;
                }
                ++pos;
                continue;
            }
            ++pos;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// PatchSlot — carries a fully-built GridEngine + auxiliary state.
// Heap-allocated on the main thread, transferred to the audio thread
// atomically.
// ---------------------------------------------------------------------------

struct PatchSlot {
    std::optional<GridEngine> engine;
    AudioInputModule*         audio_in{nullptr};
    AudioOutputModule*        audio_out{nullptr};
    // Raw pointer valid for lifetime of engine (engine owns the module).
    kairos_grid::vcv_bridge::VCVBridgeModule* vcv_tty_bridge{nullptr};
    std::vector<float>                        param_frame;
    int                                       env_tempo_id{-1}, env_beat_id{-1}, env_bar_id{-1};
    int                                       env_playing_id{-1}, env_note_id{-1}, env_gate_id{-1};
    int                                       env_velocity_id{-1};
};

// Extract the filename stem (no directory, no extension) from a path.
static std::string stem_from_path(const std::string& path) {
    const auto slash = path.rfind('/');
    const auto start = (slash == std::string::npos) ? 0u : slash + 1u;
    const auto dot   = path.rfind('.');
    const auto end   = (dot == std::string::npos || dot < start) ? path.size() : dot;
    return path.substr(start, end - start);
}

static PatchSlot* build_patch_slot(const ParsedPatch& patch, float sr) {
    const auto& reg = get_module_registry().map();

    GridGraph          g;
    AudioInputModule*  audio_in  = nullptr;
    AudioOutputModule* audio_out = nullptr;
    using kairos_grid::vcv_bridge::VCVBridgeModule;
    VCVBridgeModule* vcv_tty_bridge = nullptr;

    // Pre-pass: materialise shared storage for all :buf-id groups.  The first module
    // that references a buf_id determines the buffer size; subsequent references inherit
    // that allocation.  This allows peek and poke on the same :buf-id to share memory
    // without GridGraph needing to know about named buffers.
    //
    // FFT types: size = window/2+1 (DC to Nyquist), not a general user-specified size.
    // A peek on the same :buf-id reads directly from the live magnitude array.
    struct SharedBufs {
        std::shared_ptr<std::vector<float>> l, r;
    };

    auto fft_window_for_type = [](const std::string& t) -> std::size_t {
        if (t == "fft-512")
            return 512u;
        if (t == "fft-2048")
            return 2048u;
        if (t == "fft-4096")
            return 4096u;
        return 1024u; // "fft"
    };
    auto is_fft_type = [](const std::string& t) {
        return t == "fft" || t == "fft-512" || t == "fft-2048" || t == "fft-4096";
    };

    std::unordered_map<std::string, SharedBufs> buf_registry;
    for (const auto& pm : patch.modules) {
        if (!pm.buf_id.empty() && !buf_registry.count(pm.buf_id)) {
            std::size_t sz;
            if (is_fft_type(pm.type))
                sz = fft_window_for_type(pm.type) / 2 + 1;
            else
                sz = pm.size > 0 ? static_cast<std::size_t>(pm.size) : 2048u;
            buf_registry.emplace(pm.buf_id,
                                 SharedBufs{std::make_shared<std::vector<float>>(sz, 0.f),
                                            std::make_shared<std::vector<float>>(sz, 0.f)});
        }
    }

    for (const auto& pm : patch.modules) {
        std::unique_ptr<GridModule> mod;

        if (!pm.buf_id.empty()) {
            // Shared-buffer path: inject pre-created storage into buffer/peek/poke.
            const auto buf_it = buf_registry.find(pm.buf_id);
            if (buf_it == buf_registry.end())
                return nullptr;
            auto& [l_buf, r_buf] = buf_it->second;
            if (pm.type == "buffer") {
                mod = std::make_unique<BufferModule>(l_buf, r_buf);
            } else if (pm.type == "peek" || pm.type == "peek-nn" || pm.type == "peek-cubic") {
                const PeekInterp interp = pm.type == "peek-nn"      ? PeekInterp::None
                                          : pm.type == "peek-cubic" ? PeekInterp::Cubic
                                                                    : PeekInterp::Linear;
                mod                     = std::make_unique<PeekModule>(interp, l_buf);
            } else if (pm.type == "poke") {
                mod = std::make_unique<PokeModule>(l_buf, r_buf);
            } else if (is_fft_type(pm.type)) {
                // Shared FFT: l_buf and r_buf receive the live magnitude arrays.
                // A peek on the same :buf-id reads L-channel spectrum by CV-indexed bin.
                mod = std::make_unique<FftModule>(fft_window_for_type(pm.type), l_buf, r_buf);
            } else {
                return nullptr; // :buf-id on unsupported module type
            }
            if (!mod)
                return nullptr;
            g.add_module(std::move(mod));
            continue;
        }

        // VCV bridge — needs shm_name and n_channels; handled before registry.
        if (pm.type == "vcv-bridge") {
            auto bridge = std::make_unique<VCVBridgeModule>(pm.shm_name, pm.n_channels);
            if (!bridge)
                return nullptr;
            if (pm.shm_name == "kairos-vcv-tty")
                vcv_tty_bridge = bridge.get();
            g.add_module(std::move(bridge));
            continue;
        }

        const auto it = reg.find(pm.type);
        if (it == reg.end())
            return nullptr; // unknown type

        if (it->second.make_with_args)
            mod = it->second.make_with_args(pm.wasm_path);
        else
            mod = it->second.make();
        if (!mod)
            return nullptr;

        GridModule* raw = mod.get();

        if (pm.type == "audio-in")
            audio_in = static_cast<AudioInputModule*>(raw);
        else if (pm.type == "audio-out")
            audio_out = static_cast<AudioOutputModule*>(raw);

        if (it->second.setup_with_args)
            it->second.setup_with_args(raw, pm.type, stem_from_path(pm.wasm_path));
        else if (it->second.setup)
            it->second.setup(raw, pm.type);

        g.add_module(std::move(mod));
    }

    for (const auto& c : patch.cables)
        g.add_cable({c.from_mod, c.from_port, c.to_mod, c.to_port});

    auto res = g.build();
    if (!res.has_value())
        return nullptr;

    auto* slot           = new PatchSlot{};
    slot->engine         = std::move(*res);
    slot->audio_in       = audio_in;
    slot->audio_out      = audio_out;
    slot->vcv_tty_bridge = vcv_tty_bridge;

    slot->engine->prepare(sr);
    slot->param_frame.assign(static_cast<std::size_t>(slot->engine->port_schema().size()), 0.f);

    const auto& schema = slot->engine->port_schema();
    auto        find   = [&](const char* name) -> int {
        for (const auto& e : schema.entries)
            if (e.name == name)
                return e.id;
        return -1;
    };
    slot->env_tempo_id    = find("env/tempo_hz");
    slot->env_beat_id     = find("env/beat_phase");
    slot->env_bar_id      = find("env/bar_phase");
    slot->env_playing_id  = find("env/is_playing");
    slot->env_note_id     = find("env/voice_note");
    slot->env_gate_id     = find("env/voice_gate");
    slot->env_velocity_id = find("env/voice_velocity");

    return slot;
}

// ---------------------------------------------------------------------------
// KairosGridPlugin — one instance per plugin instantiation
// ---------------------------------------------------------------------------

class KairosGridPlugin {
  public:
    explicit KairosGridPlugin(const clap_host_t* host) : host_(host) {
        clap_plugin_.desc             = &k_descriptor;
        clap_plugin_.plugin_data      = this;
        clap_plugin_.init             = &s_init;
        clap_plugin_.destroy          = &s_destroy;
        clap_plugin_.activate         = &s_activate;
        clap_plugin_.deactivate       = &s_deactivate;
        clap_plugin_.start_processing = &s_start_processing;
        clap_plugin_.stop_processing  = &s_stop_processing;
        clap_plugin_.reset            = &s_reset;
        clap_plugin_.process          = &s_process;
        clap_plugin_.get_extension    = &s_get_extension;
        clap_plugin_.on_main_thread   = &s_on_main_thread;
    }

    const clap_plugin_t* clap_plugin() const noexcept { return &clap_plugin_; }

  private:
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    bool init() {
        build_engine(48000.f);
        return engine_.has_value();
    }

    void destroy() {
        PatchSlot* pending = pending_slot_.exchange(nullptr, std::memory_order_relaxed);
        delete pending;
        delete this;
    }

    bool activate(double sample_rate, uint32_t /*min_frames*/, uint32_t /*max_frames*/) {
        sample_rate_ = static_cast<float>(sample_rate);
        if (!engine_.has_value())
            build_engine(sample_rate_);
        else
            engine_->prepare(sample_rate_);
        const uint32_t new_size =
            static_cast<uint32_t>(std::min(static_cast<std::size_t>(engine_->port_schema().size()),
                                           static_cast<std::size_t>(kMaxParams)));
        // Only zero-fill if the param count changed (e.g. different patch).
        // Preserving existing values allows state_load() before activate()
        // to survive the activate() call with values intact.
        if (param_count_.load(std::memory_order_relaxed) != new_size) {
            for (uint32_t i = 0; i < new_size; ++i)
                param_frame_[i].store(0.f, std::memory_order_relaxed);
            param_count_.store(new_size, std::memory_order_release);
        }
        cache_port_ids();
        rebuild_tap_schema_c();
        rebuild_param_schema_c();
        return true;
    }

    void deactivate() {}

    bool start_processing() { return true; }
    void stop_processing() {}

    void reset() {
        try_install_pending_slot();
        if (engine_.has_value())
            engine_->prepare(sample_rate_);
        const uint32_t rn = param_count_.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < rn; ++i)
            param_frame_[i].store(0.f, std::memory_order_relaxed);
        rebuild_tap_schema_c();
        rebuild_param_schema_c();
    }

    // -----------------------------------------------------------------------
    // patch-bus: atomic hot-swap
    //
    // try_install_pending_slot() is called at the start of process() and reset()
    // (both audio-thread contexts).  It atomically takes ownership of any slot
    // built by push_patch_impl() on the main thread.
    //
    // Threading notes:
    //   - pending_slot_ is exchanged with nullptr (acq_rel).  The exchange is
    //     atomic; any concurrent push_patch_impl() stores will either see the
    //     exchange or have their slot taken by a subsequent call.
    //   - The old engine_ is destroyed during the move-assignment.  This is a
    //     single block of heap frees (one per module); acceptable for an
    //     infrequent patch-swap operation on a studio tool.
    //   - C-ABI schema name pointers (c_str()) are rebuilt AFTER the engine
    //     move to get stable pointers into the new engine's std::string storage.
    // -----------------------------------------------------------------------

    void try_install_pending_slot() {
        PatchSlot* next = pending_slot_.exchange(nullptr, std::memory_order_acq_rel);
        if (!next)
            return;

        // Zero the count first so the main-thread params_get_value() returns
        // false during the swap rather than reading a partially-updated frame.
        // The release ensures the audio thread's earlier writes to param_frame_
        // are not reordered past this store.
        param_count_.store(0, std::memory_order_release);

        engine_          = std::move(next->engine);
        audio_in_        = next->audio_in;
        audio_out_       = next->audio_out;
        vcv_tty_bridge_  = next->vcv_tty_bridge;
        env_tempo_id_    = next->env_tempo_id;
        env_beat_id_     = next->env_beat_id;
        env_bar_id_      = next->env_bar_id;
        env_playing_id_  = next->env_playing_id;
        env_note_id_     = next->env_note_id;
        env_gate_id_     = next->env_gate_id;
        env_velocity_id_ = next->env_velocity_id;

        // Copy the initial param values from the slot into the fixed atomic frame.
        const uint32_t n = static_cast<uint32_t>(
            std::min(next->param_frame.size(), static_cast<std::size_t>(kMaxParams)));
        for (uint32_t i = 0; i < n; ++i)
            param_frame_[i].store(next->param_frame[i], std::memory_order_relaxed);

        // Publish the new count with release: the acquire load in
        // params_get_value() synchronises-with this store, so all
        // param_frame_ element stores above are visible to the main thread.
        param_count_.store(n, std::memory_order_release);

        // Rebuild C-ABI schemas now that engine_ is in its final location.
        rebuild_tap_schema_c();
        rebuild_param_schema_c();

        delete next; // empty (moved-from) struct — one free()
    }

    bool push_patch_impl(const char* edn, uint32_t len) {
        if (!edn || len == 0)
            return false;

        const ParsedPatch parsed = parse_patch_edn(edn, len);
        if (parsed.modules.empty())
            return false;

        PatchSlot* slot = build_patch_slot(parsed, sample_rate_);
        if (!slot)
            return false;

        current_edn_.assign(edn, len);

        PatchSlot* old = pending_slot_.exchange(slot, std::memory_order_acq_rel);
        delete old; // dispose of any unprocessed pending slot (main thread)
        return true;
    }

#if defined(KAIROS_GRID_PLUGIN_HAS_WASM)
    // -----------------------------------------------------------------------
    // kairos/hot-swap extension
    //
    // Replaces the first :wasm-path value in current_edn_ with new_path, then
    // re-queues the modified EDN via push_patch_impl().  The new WasmGridModule
    // is compiled (or served from WasmModuleCache) on the main thread here;
    // the swap is installed atomically at the next process() block boundary.
    //
    // Returns false if:
    //   - new_path is unreadable
    //   - current_edn_ is empty (no patch has been pushed yet)
    //   - current_edn_ contains no :wasm-path key (patch has no WASM slot)
    // -----------------------------------------------------------------------
    // Replace one :wasm-path occurrence in current_edn_ with new_path.
    // If old_path is non-empty, match that specific slot; otherwise replace the
    // first.
    bool hot_swap_request_impl(const std::string& new_path, const std::string& old_path) {
        if (new_path.empty() || current_edn_.empty())
            return false;

        // Quick readability check before touching the patch.
        std::FILE* f = std::fopen(new_path.c_str(), "rb");
        if (!f)
            return false;
        std::fclose(f);

        const std::string key         = ":wasm-path \"";
        std::size_t       search_from = 0;

        // If old_path is given, scan for the slot whose current path matches it.
        // Otherwise fall through with search_from = 0 to replace the first slot.
        if (!old_path.empty()) {
            const std::string target = key + old_path + "\"";
            search_from              = current_edn_.find(target);
            if (search_from == std::string::npos)
                return false;
            search_from += key.size(); // point at the opening char of old_path
        }

        const std::size_t kpos = current_edn_.find(key, search_from);
        if (kpos == std::string::npos)
            return false;

        const std::size_t vs = kpos + key.size();
        const std::size_t ve = current_edn_.find('"', vs);
        if (ve == std::string::npos)
            return false;

        const std::string new_edn = current_edn_.substr(0, vs) + new_path + current_edn_.substr(ve);

        return push_patch_impl(new_edn.c_str(), static_cast<uint32_t>(new_edn.size()));
    }

    static bool s_hot_swap_request(const clap_plugin_t* p, const char* new_wasm_path,
                                   const char* old_wasm_path) noexcept {
        return cast_mut(p)->hot_swap_request_impl(std::string{new_wasm_path ? new_wasm_path : ""},
                                                  std::string{old_wasm_path ? old_wasm_path : ""});
    }
#endif

    // -----------------------------------------------------------------------
    // Audio + event processing
    // -----------------------------------------------------------------------

    clap_process_status process(const clap_process_t* proc) {
        try_install_pending_slot();

        // Drain any staged ctrl response from the control thread into VCVBridgeModule.
        // Called on audio thread; vcv_tty_bridge_ is stable here (updated above).
        if (vcv_tty_bridge_ && tty_resp_ready_.load(std::memory_order_acquire)) {
            std::string resp;
            uint8_t     rtype = 0;
            {
                std::lock_guard<std::mutex> lk(tty_resp_mu_);
                if (tty_resp_ready_.load(std::memory_order_relaxed)) {
                    resp  = std::move(tty_resp_pending_);
                    rtype = tty_resp_type_;
                    tty_resp_ready_.store(false, std::memory_order_release);
                }
            }
            if (!resp.empty())
                vcv_tty_bridge_->push_ctrl_response(rtype, resp);
        }

        if (!engine_.has_value())
            return CLAP_PROCESS_ERROR;

        if (proc->transport)
            apply_transport(proc->transport);

        const uint32_t n_events = proc->in_events->size(proc->in_events);
        for (uint32_t i = 0; i < n_events; ++i) {
            const clap_event_header_t* hdr = proc->in_events->get(proc->in_events, i);
            if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID)
                continue;
            dispatch_event(hdr);
        }

        if (audio_in_ && proc->audio_inputs_count > 0)
            audio_in_->set_buffers(proc->audio_inputs[0].data32,
                                   proc->audio_inputs[0].channel_count, proc->frames_count);
        if (audio_out_ && proc->audio_outputs_count > 0)
            audio_out_->set_buffers(proc->audio_outputs[0].data32,
                                    proc->audio_outputs[0].channel_count, proc->frames_count);

        {
            const uint32_t pc = param_count_.load(std::memory_order_relaxed);
            float          local_params[kMaxParams];
            for (uint32_t i = 0; i < pc; ++i)
                local_params[i] = param_frame_[i].load(std::memory_order_relaxed);
            engine_->apply_params({local_params, pc});
        }
        engine_->step_block(static_cast<int>(proc->frames_count));

        return CLAP_PROCESS_CONTINUE;
    }

    // -----------------------------------------------------------------------
    // Extensions
    // -----------------------------------------------------------------------

    const void* get_extension(const char* id) const {
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
            return &s_audio_ports_ext;
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
            return &s_params_ext;
        if (std::strcmp(id, CLAP_EXT_STATE) == 0)
            return &s_state_ext;
        if (std::strcmp(id, CLAP_EXT_KAIROS_TAP_BUS) == 0)
            return &s_tap_bus_ext;
        if (std::strcmp(id, CLAP_EXT_KAIROS_PARAM_BUS) == 0)
            return &s_param_bus_ext;
        if (std::strcmp(id, CLAP_EXT_KAIROS_PATCH_BUS) == 0)
            return &s_patch_bus_ext;
#if defined(KAIROS_GRID_PLUGIN_HAS_WASM)
        if (std::strcmp(id, CLAP_EXT_KAIROS_HOT_SWAP) == 0)
            return &s_hot_swap_ext;
#endif
        if (std::strcmp(id, CLAP_EXT_KAIROS_VCV_CTRL) == 0)
            return &s_vcv_ctrl_ext;
        return nullptr;
    }

    void on_main_thread() {}

    // -----------------------------------------------------------------------
    // Audio ports extension
    // -----------------------------------------------------------------------

    static uint32_t audio_ports_count(const clap_plugin_t* /*p*/, bool /*is_input*/) { return 1; }

    static bool audio_ports_get(const clap_plugin_t* /*p*/, uint32_t index, bool is_input,
                                clap_audio_port_info_t* info) {
        if (index != 0)
            return false;
        info->id            = 0;
        info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type     = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "%s", is_input ? "Stereo In" : "Stereo Out");
        return true;
    }

    // -----------------------------------------------------------------------
    // Params extension
    // -----------------------------------------------------------------------

    static uint32_t params_count(const clap_plugin_t* p) {
        return cast(p)->param_count_.load(std::memory_order_relaxed);
    }

    static bool params_get_info(const clap_plugin_t* p, uint32_t index, clap_param_info_t* info) {
        const auto* self = cast(p);
        if (!self->engine_.has_value())
            return false;
        const auto& schema = self->engine_->port_schema();
        if (index >= static_cast<uint32_t>(schema.size()))
            return false;
        const auto& e       = schema.entries[index];
        info->id            = static_cast<clap_id>(e.id);
        info->flags         = CLAP_PARAM_IS_AUTOMATABLE;
        info->cookie        = nullptr;
        info->min_value     = 0.0;
        info->max_value     = 1.0;
        info->default_value = 0.0;
        const char* slash   = std::strrchr(e.name.c_str(), '/');
        const char* leaf    = slash ? slash + 1 : e.name.c_str();
        std::snprintf(info->name, sizeof(info->name), "%s", leaf);
        std::snprintf(info->module, sizeof(info->module), "kairos-grid");
        return true;
    }

    // [main-thread] — safe: param_count_ acquire-load synchronises-with the
    // release-store in try_install_pending_slot(), so all param_frame_ stores
    // before that release are visible here.  param_frame_ elements are
    // std::atomic<float> so the relaxed load is race-free.
    static bool params_get_value(const clap_plugin_t* p, clap_id param_id, double* out) {
        const auto*    self  = cast(p);
        const uint32_t count = self->param_count_.load(std::memory_order_acquire);
        const uint32_t idx   = static_cast<uint32_t>(param_id);
        if (count == 0 || idx >= count)
            return false;
        *out = static_cast<double>(self->param_frame_[idx].load(std::memory_order_relaxed));
        return true;
    }

    static bool params_value_to_text(const clap_plugin_t* /*p*/, clap_id /*id*/, double value,
                                     char* buf, uint32_t cap) {
        std::snprintf(buf, cap, "%.4f", value);
        return true;
    }

    static bool params_text_to_value(const clap_plugin_t* /*p*/, clap_id /*id*/, const char* text,
                                     double* out) {
        char* end = nullptr;
        *out      = std::strtod(text, &end);
        return end != text;
    }

    static void params_flush(const clap_plugin_t* p, const clap_input_events_t* in,
                             const clap_output_events_t* /*out*/) {
        auto* self = cast_mut(p);
        if (!self->engine_.has_value())
            return;
        const uint32_t n = in->size(in);
        for (uint32_t i = 0; i < n; ++i) {
            const clap_event_header_t* hdr = in->get(in, i);
            if (hdr->space_id == CLAP_CORE_EVENT_SPACE_ID)
                self->dispatch_event(hdr);
        }
        const uint32_t pc = self->param_count_.load(std::memory_order_relaxed);
        float          local_params[kMaxParams];
        for (uint32_t i = 0; i < pc; ++i)
            local_params[i] = self->param_frame_[i].load(std::memory_order_relaxed);
        self->engine_->apply_params({local_params, pc});
    }

    // -----------------------------------------------------------------------
    // State extension
    //
    // Binary format (all fields little-endian):
    //   uint32_t  magic   = 0x4B475354  ('KGST')
    //   uint32_t  version = 1
    //   uint32_t  n_params
    //   for each param:
    //     uint32_t  name_len
    //     char      name[name_len]   (no null terminator)
    //     float     value
    // -----------------------------------------------------------------------

    static bool stream_write_all(const clap_ostream_t* s, const void* buf, uint64_t n) {
        const auto* p       = static_cast<const uint8_t*>(buf);
        uint64_t    written = 0;
        while (written < n) {
            const int64_t r = s->write(s, p + written, n - written);
            if (r <= 0)
                return false;
            written += static_cast<uint64_t>(r);
        }
        return true;
    }

    static bool stream_read_all(const clap_istream_t* s, void* buf, uint64_t n) {
        auto*    p    = static_cast<uint8_t*>(buf);
        uint64_t done = 0;
        while (done < n) {
            const int64_t r = s->read(s, p + done, n - done);
            if (r <= 0)
                return false;
            done += static_cast<uint64_t>(r);
        }
        return true;
    }

    static bool state_save(const clap_plugin_t* p, const clap_ostream_t* s) {
        const auto* self = cast(p);
        if (!self->engine_.has_value())
            return false;

        constexpr uint32_t k_magic   = 0x4B475354u;
        constexpr uint32_t k_version = 1u;
        const auto&        schema    = self->engine_->port_schema();
        const auto         n_params  = static_cast<uint32_t>(schema.size());

        if (!stream_write_all(s, &k_magic, sizeof(k_magic)))
            return false;
        if (!stream_write_all(s, &k_version, sizeof(k_version)))
            return false;
        if (!stream_write_all(s, &n_params, sizeof(n_params)))
            return false;

        for (const auto& e : schema.entries) {
            const auto  name_len = static_cast<uint32_t>(e.name.size());
            const auto  eid      = static_cast<uint32_t>(e.id);
            const float value    = (eid < self->param_count_.load(std::memory_order_relaxed))
                                       ? self->param_frame_[eid].load(std::memory_order_relaxed)
                                       : 0.f;
            if (!stream_write_all(s, &name_len, sizeof(name_len)))
                return false;
            if (name_len > 0 && !stream_write_all(s, e.name.data(), name_len))
                return false;
            if (!stream_write_all(s, &value, sizeof(value)))
                return false;
        }
        return true;
    }

    static bool state_load(const clap_plugin_t* p, const clap_istream_t* s) {
        auto* self = cast_mut(p);
        if (!self->engine_.has_value())
            return false;

        constexpr uint32_t k_magic   = 0x4B475354u;
        constexpr uint32_t k_version = 1u;

        uint32_t magic{}, version{}, n_params{};
        if (!stream_read_all(s, &magic, sizeof(magic)))
            return false;
        if (!stream_read_all(s, &version, sizeof(version)))
            return false;
        if (magic != k_magic || version != k_version)
            return false;
        if (!stream_read_all(s, &n_params, sizeof(n_params)))
            return false;

        std::string name_buf;
        for (uint32_t i = 0; i < n_params; ++i) {
            uint32_t name_len{};
            if (!stream_read_all(s, &name_len, sizeof(name_len)))
                return false;
            name_buf.resize(name_len);
            if (name_len > 0 && !stream_read_all(s, name_buf.data(), name_len))
                return false;
            float value{};
            if (!stream_read_all(s, &value, sizeof(value)))
                return false;
            self->set_param(self->find_port(name_buf.c_str()), value);
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // kairos/tap-bus extension
    // -----------------------------------------------------------------------

    void rebuild_tap_schema_c() {
        if (!engine_.has_value()) {
            tap_entries_c_.clear();
            tap_schema_c_ = {0, 0, nullptr};
            return;
        }
        const auto& ts = engine_->tap_schema();
        tap_entries_c_.resize(ts.entries.size());
        for (std::size_t i = 0; i < ts.entries.size(); ++i) {
            tap_entries_c_[i].id   = static_cast<uint32_t>(ts.entries[i].id);
            tap_entries_c_[i].name = ts.entries[i].name.c_str();
        }
        tap_schema_c_.epoch   = ts.epoch;
        tap_schema_c_.count   = static_cast<uint32_t>(tap_entries_c_.size());
        tap_schema_c_.entries = tap_entries_c_.empty() ? nullptr : tap_entries_c_.data();
    }

    static const clap_kairos_tap_schema_t* tap_bus_get_schema(const clap_plugin_t* p) {
        return &cast(p)->tap_schema_c_;
    }

    static const float* tap_bus_get_tap_frame(const clap_plugin_t* p, uint32_t* out_count) {
        const auto* self = cast(p);
        *out_count       = 0;
        if (!self->engine_.has_value())
            return nullptr;
        const auto frame = self->engine_->tap_frame();
        *out_count       = static_cast<uint32_t>(frame.size());
        return frame.data();
    }

    // -----------------------------------------------------------------------
    // kairos/param-bus extension
    // -----------------------------------------------------------------------

    void rebuild_param_schema_c() {
        if (!engine_.has_value()) {
            param_entries_c_.clear();
            param_schema_c_ = {0, 0, nullptr};
            return;
        }
        const auto& ps = engine_->port_schema();
        param_entries_c_.resize(ps.entries.size());
        for (std::size_t i = 0; i < ps.entries.size(); ++i) {
            param_entries_c_[i].id   = static_cast<uint32_t>(ps.entries[i].id);
            param_entries_c_[i].name = ps.entries[i].name.c_str();
        }
        param_schema_c_.epoch   = ps.epoch;
        param_schema_c_.count   = static_cast<uint32_t>(param_entries_c_.size());
        param_schema_c_.entries = param_entries_c_.empty() ? nullptr : param_entries_c_.data();
    }

    static const clap_kairos_param_schema_t* param_bus_get_schema(const clap_plugin_t* p) {
        return &cast(p)->param_schema_c_;
    }

    static bool param_bus_set_param_frame(const clap_plugin_t* p, const float* values,
                                          uint32_t count) {
        auto*          self = cast_mut(p);
        const uint32_t n    = std::min(count, self->param_count_.load(std::memory_order_relaxed));
        if (n == 0)
            return false;
        for (uint32_t i = 0; i < n; ++i)
            self->param_frame_[i].store(values[i], std::memory_order_relaxed);
        return true;
    }

    // -----------------------------------------------------------------------
    // kairos/patch-bus extension
    // -----------------------------------------------------------------------

    static bool patch_bus_push_patch(const clap_plugin_t* p, const char* edn, uint32_t len) {
        return cast_mut(p)->push_patch_impl(edn, len);
    }

    static const char* patch_bus_get_patch(const clap_plugin_t* p) {
        const auto& edn = cast(p)->current_edn_;
        return edn.empty() ? nullptr : edn.c_str();
    }

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    void build_engine(float sr) {
        GridGraph g;

        auto               audio_in_up  = std::make_unique<AudioInputModule>();
        auto               audio_out_up = std::make_unique<AudioOutputModule>();
        AudioInputModule*  raw_in       = audio_in_up.get();
        AudioOutputModule* raw_out      = audio_out_up.get();

        g.add_module(std::make_unique<EnvironmentModule>());
        const int in_idx  = g.add_module(std::move(audio_in_up));
        const int out_idx = g.add_module(std::move(audio_out_up));

        g.add_cable({in_idx, AudioInputModule::k_left, out_idx, AudioOutputModule::k_left});
        g.add_cable({in_idx, AudioInputModule::k_right, out_idx, AudioOutputModule::k_right});

        auto res = g.build();
        if (!res.has_value())
            return;
        engine_    = std::move(*res);
        audio_in_  = raw_in;
        audio_out_ = raw_out;
        engine_->prepare(sr);
        {
            const uint32_t n = static_cast<uint32_t>(
                std::min(static_cast<std::size_t>(engine_->port_schema().size()),
                         static_cast<std::size_t>(kMaxParams)));
            for (uint32_t i = 0; i < n; ++i)
                param_frame_[i].store(0.f, std::memory_order_relaxed);
            param_count_.store(n, std::memory_order_release);
        }

        cache_port_ids();
        rebuild_tap_schema_c();
        rebuild_param_schema_c();
    }

    void cache_port_ids() {
        env_tempo_id_    = find_port("env/tempo_hz");
        env_beat_id_     = find_port("env/beat_phase");
        env_bar_id_      = find_port("env/bar_phase");
        env_playing_id_  = find_port("env/is_playing");
        env_note_id_     = find_port("env/voice_note");
        env_gate_id_     = find_port("env/voice_gate");
        env_velocity_id_ = find_port("env/voice_velocity");
    }

    int find_port(const char* name) const {
        if (!engine_.has_value())
            return -1;
        for (const auto& e : engine_->port_schema().entries)
            if (e.name == name)
                return e.id;
        return -1;
    }

    void set_param(int id, float value) {
        const auto uid = static_cast<uint32_t>(id);
        if (id >= 0 && uid < param_count_.load(std::memory_order_relaxed))
            param_frame_[uid].store(value, std::memory_order_relaxed);
    }

    void apply_transport(const clap_event_transport_t* t) {
        if (t->flags & CLAP_TRANSPORT_HAS_TEMPO)
            set_param(env_tempo_id_, static_cast<float>(t->tempo / 60.0));

        set_param(env_playing_id_, (t->flags & CLAP_TRANSPORT_IS_PLAYING) ? 1.f : 0.f);

        if (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) {
            const double pos = static_cast<double>(t->song_pos_beats) / CLAP_BEATTIME_FACTOR;
            set_param(env_beat_id_, static_cast<float>(pos - std::floor(pos)));

            if (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) {
                const double bar_start = static_cast<double>(t->bar_start) / CLAP_BEATTIME_FACTOR;
                const double beats_per_bar = static_cast<double>(t->tsig_num);
                if (beats_per_bar > 0.0) {
                    const double bar_phase = (pos - bar_start) / beats_per_bar;
                    set_param(env_bar_id_, static_cast<float>(std::clamp(bar_phase, 0.0, 1.0)));
                }
            }
        }
    }

    void dispatch_event(const clap_event_header_t* hdr) {
        switch (hdr->type) {
        case CLAP_EVENT_TRANSPORT:
            apply_transport(reinterpret_cast<const clap_event_transport_t*>(hdr));
            break;
        case CLAP_EVENT_NOTE_ON: {
            const auto* ev = reinterpret_cast<const clap_event_note_t*>(hdr);
            set_param(env_note_id_, static_cast<float>(ev->key));
            set_param(env_velocity_id_, static_cast<float>(ev->velocity * 127.0));
            set_param(env_gate_id_, 1.f);
            break;
        }
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE:
            set_param(env_gate_id_, 0.f);
            break;
        case CLAP_EVENT_PARAM_VALUE: {
            const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
            set_param(static_cast<int>(ev->param_id), static_cast<float>(ev->value));
            break;
        }
        default:
            break;
        }
    }

    // -----------------------------------------------------------------------
    // Trampolines
    // -----------------------------------------------------------------------

    static const KairosGridPlugin* cast(const clap_plugin_t* p) {
        return static_cast<const KairosGridPlugin*>(p->plugin_data);
    }
    static KairosGridPlugin* cast_mut(const clap_plugin_t* p) {
        return static_cast<KairosGridPlugin*>(p->plugin_data);
    }

    static bool s_init(const clap_plugin_t* p) { return cast_mut(p)->init(); }
    static void s_destroy(const clap_plugin_t* p) { cast_mut(p)->destroy(); }
    static bool s_activate(const clap_plugin_t* p, double sr, uint32_t mn, uint32_t mx) {
        return cast_mut(p)->activate(sr, mn, mx);
    }
    static void s_deactivate(const clap_plugin_t* p) { cast_mut(p)->deactivate(); }
    static bool s_start_processing(const clap_plugin_t* p) {
        return cast_mut(p)->start_processing();
    }
    static void s_stop_processing(const clap_plugin_t* p) { cast_mut(p)->stop_processing(); }
    static void s_reset(const clap_plugin_t* p) { cast_mut(p)->reset(); }
    static clap_process_status s_process(const clap_plugin_t* p, const clap_process_t* proc) {
        return cast_mut(p)->process(proc);
    }
    static const void* s_get_extension(const clap_plugin_t* p, const char* id) {
        return cast(p)->get_extension(id);
    }
    static void s_on_main_thread(const clap_plugin_t* p) { cast_mut(p)->on_main_thread(); }

    // VCV ctrl extension — called from the kairos control thread (not audio thread).
    static void s_vcv_ctrl_push(const clap_plugin_t* p, uint8_t type, const char* payload,
                                uint32_t payload_len) noexcept {
        auto* kg = cast_mut(p);
        {
            std::lock_guard<std::mutex> lk(kg->tty_resp_mu_);
            kg->tty_resp_pending_.assign(payload, payload_len);
            kg->tty_resp_type_ = type;
        }
        kg->tty_resp_ready_.store(true, std::memory_order_release);
    }

    // -----------------------------------------------------------------------
    // Static extension vtables
    // -----------------------------------------------------------------------

    static const clap_plugin_audio_ports_t s_audio_ports_ext;
    static const clap_plugin_params_t      s_params_ext;
    static const clap_plugin_state_t       s_state_ext;
    static const clap_plugin_tap_bus_t     s_tap_bus_ext;
    static const clap_plugin_param_bus_t   s_param_bus_ext;
    static const clap_plugin_patch_bus_t   s_patch_bus_ext;
#if defined(KAIROS_GRID_PLUGIN_HAS_WASM)
    static const clap_kairos_hot_swap_t s_hot_swap_ext;
#endif
    static const clap_kairos_vcv_ctrl_t s_vcv_ctrl_ext;

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    // Maximum number of param-bus entries any patch graph can expose.
    // All patches are bounded well below this; it is a hard cap, not a typical
    // size.
    static constexpr uint32_t kMaxParams = 256;

    [[maybe_unused]] const clap_host_t* host_;
    clap_plugin_t                       clap_plugin_{};
    std::optional<GridEngine>           engine_;
    AudioInputModule*                   audio_in_{nullptr};
    AudioOutputModule*                  audio_out_{nullptr};
    // Raw pointer to the TTY VCVBridgeModule (if patch contains one).
    // Audio-thread only: updated in try_install_pending_slot, read in process().
    kairos_grid::vcv_bridge::VCVBridgeModule* vcv_tty_bridge_{nullptr};
    // Ctrl response staged by control thread; drained into vcv_tty_bridge_ in process().
    std::mutex        tty_resp_mu_;
    std::string       tty_resp_pending_;
    uint8_t           tty_resp_type_{0};
    std::atomic<bool> tty_resp_ready_{false};

    // param_frame_ and param_count_ are written by the audio thread
    // (try_install_pending_slot, set_param, param_bus_set_param_frame) and read
    // by the main thread (params_get_value, params_count).  Using a fixed-size
    // atomic array + a separate atomic count eliminates the use-after-free window
    // that existed when param_frame_ was a std::vector<float>: a move-assignment
    // on the audio thread could free the old backing buffer while the main thread
    // held a stale pointer to it.  See kairos-grid issue #1.
    //
    // TODO(follow-up): params_get_info() and params_count_c still read engine_
    // without synchronisation; they are safe today only because hosts call them
    // before start_processing() or after a params_rescan notification that has
    // not yet been implemented.  Wire request_callback + params_rescan to fix.
    std::atomic<float>    param_frame_[kMaxParams];
    std::atomic<uint32_t> param_count_{0};

    float       sample_rate_{48000.f};
    std::string current_edn_; // main-thread only

    // Pending patch slot — written by main thread, consumed by audio thread
    std::atomic<PatchSlot*> pending_slot_{nullptr};

    // C-ABI tap schema snapshot
    clap_kairos_tap_schema_t             tap_schema_c_{};
    std::vector<clap_kairos_tap_entry_t> tap_entries_c_;

    // C-ABI param schema snapshot
    clap_kairos_param_schema_t             param_schema_c_{};
    std::vector<clap_kairos_param_entry_t> param_entries_c_;

    int env_tempo_id_{-1};
    int env_beat_id_{-1};
    int env_bar_id_{-1};
    int env_playing_id_{-1};
    int env_note_id_{-1};
    int env_gate_id_{-1};
    int env_velocity_id_{-1};
};

const clap_plugin_audio_ports_t KairosGridPlugin::s_audio_ports_ext = {
    .count = &KairosGridPlugin::audio_ports_count,
    .get   = &KairosGridPlugin::audio_ports_get,
};

const clap_plugin_params_t KairosGridPlugin::s_params_ext = {
    .count         = &KairosGridPlugin::params_count,
    .get_info      = &KairosGridPlugin::params_get_info,
    .get_value     = &KairosGridPlugin::params_get_value,
    .value_to_text = &KairosGridPlugin::params_value_to_text,
    .text_to_value = &KairosGridPlugin::params_text_to_value,
    .flush         = &KairosGridPlugin::params_flush,
};

const clap_plugin_state_t KairosGridPlugin::s_state_ext = {
    .save = &KairosGridPlugin::state_save,
    .load = &KairosGridPlugin::state_load,
};

const clap_plugin_tap_bus_t KairosGridPlugin::s_tap_bus_ext = {
    .get_schema    = &KairosGridPlugin::tap_bus_get_schema,
    .get_tap_frame = &KairosGridPlugin::tap_bus_get_tap_frame,
};

const clap_plugin_param_bus_t KairosGridPlugin::s_param_bus_ext = {
    .get_schema      = &KairosGridPlugin::param_bus_get_schema,
    .set_param_frame = &KairosGridPlugin::param_bus_set_param_frame,
};

const clap_plugin_patch_bus_t KairosGridPlugin::s_patch_bus_ext = {
    .push_patch = &KairosGridPlugin::patch_bus_push_patch,
    .get_patch  = &KairosGridPlugin::patch_bus_get_patch,
};

#if defined(KAIROS_GRID_PLUGIN_HAS_WASM)
const clap_kairos_hot_swap_t KairosGridPlugin::s_hot_swap_ext = {
    .request = &KairosGridPlugin::s_hot_swap_request,
};
#endif

const clap_kairos_vcv_ctrl_t KairosGridPlugin::s_vcv_ctrl_ext = {
    .push_ctrl_response = &KairosGridPlugin::s_vcv_ctrl_push,
};

// ---------------------------------------------------------------------------
// Plugin factory
// ---------------------------------------------------------------------------

static uint32_t factory_count(const clap_plugin_factory_t*) {
    return 1;
}

static const clap_plugin_descriptor_t* factory_get_descriptor(const clap_plugin_factory_t*,
                                                              uint32_t index) {
    return index == 0 ? &k_descriptor : nullptr;
}

static const clap_plugin_t* factory_create(const clap_plugin_factory_t*, const clap_host_t* host,
                                           const char* plugin_id) {
    if (std::strcmp(plugin_id, k_descriptor.id) != 0)
        return nullptr;
    return (new KairosGridPlugin(host))->clap_plugin();
}

static const clap_plugin_factory_t k_factory = {
    .get_plugin_count      = &factory_count,
    .get_plugin_descriptor = &factory_get_descriptor,
    .create_plugin         = &factory_create,
};

} // namespace kairos_grid

// ---------------------------------------------------------------------------
// CLAP entry point (C linkage, visible to the host's dynamic linker)
// ---------------------------------------------------------------------------

extern "C" {

// Per-extension registry context: wraps ModuleRegistryImpl with the kgext path
// so extensions can locate co-located companions via companion_path(".kgnwt").
struct ExtensionRegistryContext : kairos_grid::GridModuleRegistry {
    kairos_grid::ModuleRegistryImpl& inner;
    std::filesystem::path            kgext_path;

    ExtensionRegistryContext(kairos_grid::ModuleRegistryImpl& r, std::filesystem::path p)
        : inner(r), kgext_path(std::move(p)) {}

    void add(std::string key, kairos_grid::ModuleSpec spec) override {
        inner.add(std::move(key), std::move(spec));
    }

    std::string companion_path(const std::string& ext) const override {
        auto            candidate = kgext_path;
        std::error_code ec;
        candidate.replace_extension(ext);
        if (std::filesystem::exists(candidate, ec))
            return candidate.string();
        return {};
    }
};

static void load_kgext_extensions(kairos_grid::ModuleRegistryImpl& reg, const char* plugin_path) {
    if (!plugin_path)
        return;

    // Look for *.kgext files in the same directory as the .clap bundle.
    // A .kgext is a shared library exporting kairos_grid_extension_entry.
    std::error_code ec;
    auto            dir = std::filesystem::path(plugin_path).parent_path();
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".kgext")
            continue;
        void* handle = dlopen(entry.path().c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            std::fprintf(stderr, "kairos-grid: dlopen %s failed: %s\n", entry.path().c_str(),
                         dlerror());
            continue;
        }
        auto* fn = reinterpret_cast<kairos_grid_extension_entry_fn*>(
            dlsym(handle, KAIROS_GRID_EXTENSION_ENTRY));
        if (fn) {
            ExtensionRegistryContext ctx(reg, entry.path());
            fn(ctx);
        } else {
            std::fprintf(stderr, "kairos-grid: %s has no %s symbol\n", entry.path().c_str(),
                         KAIROS_GRID_EXTENSION_ENTRY);
            dlclose(handle);
        }
    }
}

static bool entry_init(const char* plugin_path) {
    auto& reg = kairos_grid::get_module_registry();
    kairos_grid::populate_builtins(reg);
    load_kgext_extensions(reg, plugin_path);
    return true;
}
static void entry_deinit() {
}
static const void* entry_get_factory(const char* factory_id) {
    if (std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &kairos_grid::k_factory;
    return nullptr;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init         = &entry_init,
    .deinit       = &entry_deinit,
    .get_factory  = &entry_get_factory,
};

} // extern "C"
