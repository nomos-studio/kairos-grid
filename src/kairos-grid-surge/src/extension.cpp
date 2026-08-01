// SPDX-License-Identifier: GPL-3.0-or-later
// kairos-grid-surge.kgext — Surge XT module registrations.
// GPL-3.0 because SurgeXT and its dependencies are GPL-3.0.
// Built as a standalone .kgext and loaded at runtime by the kairos-grid CLAP plugin.
#include <kairos_grid/grid_extension.hpp>
#include <kairos_grid/surge/surge_effect_module.hpp>
#include <kairos_grid/surge/surge_filter_module.hpp>
#include <kairos_grid/surge/surge_modulator_modules.hpp>
#include <kairos_grid/surge/surge_osc_module.hpp>
#include <kairos_grid/surge/surge_waveshaper_module.hpp>

#include <memory>
#include <string>

using namespace kairos_grid;

extern "C" void kairos_grid_extension_entry(GridModuleRegistry& r) {
    // Shared setup lambdas.
    // Filters: cutoff at port 2, resonance at port 3.
    const auto filter_setup = [](GridModule* m, const std::string& pfx) {
        m->param_ports = {{pfx + "/cutoff", 2}, {pfx + "/resonance", 3}};
    };
    // Effects: n_fx_params params at ports 2..2+n_fx_params-1.
    const auto effect_setup = [](GridModule* m, const std::string& pfx) {
        m->param_ports.clear();
        for (int i = 0; i < n_fx_params; ++i)
            m->param_ports.push_back({pfx + "/p" + std::to_string(i), 2 + i});
    };
    // Oscillators: n_osc_params params at ports 1..1+n_osc_params-1.
    const auto osc_setup = [](GridModule* m, const std::string& pfx) {
        m->param_ports.clear();
        for (int i = 0; i < n_osc_params; ++i)
            m->param_ports.push_back({pfx + "/p" + std::to_string(i), 1 + i});
    };

    // ---------------------------------------------------------------------------
    // sst-filters
    // ---------------------------------------------------------------------------
    r.add("ladder", {[]() -> std::unique_ptr<GridModule> {
                         return std::make_unique<surge::VintageLadderModule>();
                     },
                     filter_setup, nullptr, nullptr});
    r.add("diode", {[]() -> std::unique_ptr<GridModule> {
                        return std::make_unique<surge::DiodeLadderModule>();
                    },
                    filter_setup, nullptr, nullptr});
    r.add("k35-lp",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::K35LPModule>(); },
           filter_setup, nullptr, nullptr});
    r.add("k35-hp",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::K35HPModule>(); },
           filter_setup, nullptr, nullptr});
    r.add("obxd-4p", {[]() -> std::unique_ptr<GridModule> {
                          return std::make_unique<surge::OBXD4PoleModule>();
                      },
                      filter_setup, nullptr, nullptr});
    r.add("lp12",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::LP12Module>(); },
           filter_setup, nullptr, nullptr});
    r.add("lp24",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::LP24Module>(); },
           filter_setup, nullptr, nullptr});
    r.add("hp12",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::HP12Module>(); },
           filter_setup, nullptr, nullptr});
    r.add("hp24",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::HP24Module>(); },
           filter_setup, nullptr, nullptr});
    r.add("bp12",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::BP12Module>(); },
           filter_setup, nullptr, nullptr});
    r.add("bp24",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::BP24Module>(); },
           filter_setup, nullptr, nullptr});

    // ---------------------------------------------------------------------------
    // Surge effects
    // ---------------------------------------------------------------------------
    r.add("reverb1",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::Reverb1Module>(); },
           effect_setup, nullptr, nullptr});
    r.add("reverb2",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::Reverb2Module>(); },
           effect_setup, nullptr, nullptr});
    r.add("chorus",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::ChorusModule>(); },
           effect_setup, nullptr, nullptr});
    r.add("delay",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::DelayModule>(); },
           effect_setup, nullptr, nullptr});
    r.add("phaser",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::PhaserModule>(); },
           effect_setup, nullptr, nullptr});
    r.add("flanger",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::FlangerModule>(); },
           effect_setup, nullptr, nullptr});
    r.add("bonsai",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::BonsaiModule>(); },
           effect_setup, nullptr, nullptr});
    r.add("ensemble", {[]() -> std::unique_ptr<GridModule> {
                           return std::make_unique<surge::EnsembleModule>();
                       },
                       effect_setup, nullptr, nullptr});
    r.add("distortion", {[]() -> std::unique_ptr<GridModule> {
                             return std::make_unique<surge::DistortionModule>();
                         },
                         effect_setup, nullptr, nullptr});

    // ---------------------------------------------------------------------------
    // Surge oscillators
    // ---------------------------------------------------------------------------
    r.add("classic", {[]() -> std::unique_ptr<GridModule> {
                          return std::make_unique<surge::ClassicOscModule>();
                      },
                      osc_setup, nullptr, nullptr});
    r.add("sine-osc",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::SineOscModule>(); },
           osc_setup, nullptr, nullptr});
    r.add("modern", {[]() -> std::unique_ptr<GridModule> {
                         return std::make_unique<surge::ModernOscModule>();
                     },
                     osc_setup, nullptr, nullptr});
    r.add("fm2",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::FM2OscModule>(); },
           osc_setup, nullptr, nullptr});
    r.add("fm3",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::FM3OscModule>(); },
           osc_setup, nullptr, nullptr});
    r.add("string", {[]() -> std::unique_ptr<GridModule> {
                         return std::make_unique<surge::StringOscModule>();
                     },
                     osc_setup, nullptr, nullptr});
    r.add("twist", {[]() -> std::unique_ptr<GridModule> {
                        return std::make_unique<surge::TwistOscModule>();
                    },
                    osc_setup, nullptr, nullptr});

    // ---------------------------------------------------------------------------
    // Surge modulators — param_ports declared in constructor; no setup lambda.
    // ---------------------------------------------------------------------------
    r.add("adsr",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<surge::ADSRModule>(); },
           nullptr, nullptr, nullptr});
    r.add("lfo", {[]() -> std::unique_ptr<GridModule> {
                      return std::make_unique<surge::SimpleLFOModule>();
                  },
                  nullptr, nullptr, nullptr});

    // ---------------------------------------------------------------------------
    // sst-waveshapers — curated palette via 4-sample SIMD buffer.
    // Port layout: in-l(0), in-r(1), drive(2, 0=unity, 1=16×).  4-sample latency.
    // ---------------------------------------------------------------------------
    using WS     = sst::waveshapers::WaveshaperType;
    auto ws_make = [](WS t) {
        return [t]() -> std::unique_ptr<GridModule> {
            return std::make_unique<surge::SurgeWaveshaperModule>(t);
        };
    };
    auto ws_setup = [](GridModule* m, const std::string& pfx) {
        m->param_ports = {{pfx + "/drive", 2}};
    };

    // Saturators
    r.add("sst-soft", {ws_make(WS::wst_soft), ws_setup, nullptr, nullptr});
    r.add("sst-hard", {ws_make(WS::wst_hard), ws_setup, nullptr, nullptr});
    r.add("sst-asym", {ws_make(WS::wst_asym), ws_setup, nullptr, nullptr});
    r.add("sst-medium", {ws_make(WS::wst_zamsat), ws_setup, nullptr, nullptr});
    r.add("sst-ojd", {ws_make(WS::wst_ojd), ws_setup, nullptr, nullptr});

    // Fuzz
    r.add("sst-fuzz", {ws_make(WS::wst_fuzz), ws_setup, nullptr, nullptr});
    r.add("sst-fuzz-heavy", {ws_make(WS::wst_fuzzheavy), ws_setup, nullptr, nullptr});

    // Wavefolders
    r.add("sst-westfold", {ws_make(WS::wst_westfold), ws_setup, nullptr, nullptr});
    r.add("sst-dualfold", {ws_make(WS::wst_dualfold), ws_setup, nullptr, nullptr});
    r.add("sst-softfold", {ws_make(WS::wst_softfold), ws_setup, nullptr, nullptr});

    // Harmonic shapers (Chebyshev)
    r.add("sst-harmonic2", {ws_make(WS::wst_cheby2), ws_setup, nullptr, nullptr});
    r.add("sst-harmonic3", {ws_make(WS::wst_cheby3), ws_setup, nullptr, nullptr});
}
