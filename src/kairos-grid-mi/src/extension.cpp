// SPDX-License-Identifier: LGPL-2.1-or-later
// kairos-grid-mi.kgext — MIT-licensed MI DSP module registrations.
// Built as a standalone .kgext and loaded at runtime by the kairos-grid CLAP plugin.
#include <kairos_grid/grid_extension.hpp>
#include <kairos_grid/mi/lpg_module.hpp>
#include <kairos_grid/mi/one_pole_module.hpp>
#include <kairos_grid/mi/plaits_module.hpp>
#include <kairos_grid/mi/svf_module.hpp>
#include <kairos_grid/mi/waveshaper_module.hpp>

#include <memory>
#include <string>

using namespace kairos_grid;

extern "C" void kairos_grid_extension_entry(GridModuleRegistry& r) {
    r.add("plaits",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<mi::PlaitsModule>(); },
           [](GridModule* m, const std::string& pfx) {
               m->param_ports = {
                   {pfx + "/harmonics", 1}, {pfx + "/timbre", 2}, {pfx + "/morph", 3},
                   {pfx + "/engine", 6},    {pfx + "/level", 5},
               };
           },
           nullptr, nullptr});
    r.add("svf", {[]() -> std::unique_ptr<GridModule> { return std::make_unique<mi::SvfModule>(); },
                  [](GridModule* m, const std::string& pfx) {
                      m->param_ports = {
                          {pfx + "/cutoff", 1},
                          {pfx + "/q", 2},
                      };
                  },
                  nullptr, nullptr});
    r.add("one-pole",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<mi::OnePoleModule>(); },
           [](GridModule* m, const std::string& pfx) { m->param_ports = {{pfx + "/freq", 1}}; },
           nullptr, nullptr});
    // Low-pass gate variants: "lpg" is clean (cv maps directly); "lpg-vactrol"
    // adds asymmetric one-pole dynamics on the cv path with per-instance spread.
    r.add("lpg",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<mi::LpgModule>(false); },
           [](GridModule* m, const std::string& pfx) {
               m->param_ports = {{pfx + "/cv", 2}, {pfx + "/character", 4}};
           },
           nullptr, nullptr});
    r.add("lpg-vactrol",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<mi::LpgModule>(true); },
           [](GridModule* m, const std::string& pfx) {
               m->param_ports = {{pfx + "/cv", 2}, {pfx + "/decay", 3}, {pfx + "/character", 4}};
           },
           nullptr, nullptr});
    // Scalar ADAA waveshapers — four shapes, no oversampling.
    // Inputs: in-l(0), in-r(1), drive(2, 0=unity, 1=16×).
    r.add("ws-hard",
          {[]() -> std::unique_ptr<GridModule> {
               return std::make_unique<mi::WaveshaperModule>(&mi::WaveshaperModule::f_hard,
                                                             &mi::WaveshaperModule::F_hard);
           },
           [](GridModule* m, const std::string& pfx) { m->param_ports = {{pfx + "/drive", 2}}; },
           nullptr, nullptr});
    r.add("ws-tanh",
          {[]() -> std::unique_ptr<GridModule> {
               return std::make_unique<mi::WaveshaperModule>(&mi::WaveshaperModule::f_tanh,
                                                             &mi::WaveshaperModule::F_tanh);
           },
           [](GridModule* m, const std::string& pfx) { m->param_ports = {{pfx + "/drive", 2}}; },
           nullptr, nullptr});
    r.add("ws-soft",
          {[]() -> std::unique_ptr<GridModule> {
               return std::make_unique<mi::WaveshaperModule>(&mi::WaveshaperModule::f_soft,
                                                             &mi::WaveshaperModule::F_soft);
           },
           [](GridModule* m, const std::string& pfx) { m->param_ports = {{pfx + "/drive", 2}}; },
           nullptr, nullptr});
    r.add("ws-fold",
          {[]() -> std::unique_ptr<GridModule> {
               return std::make_unique<mi::WaveshaperModule>(&mi::WaveshaperModule::f_fold,
                                                             &mi::WaveshaperModule::F_fold);
           },
           [](GridModule* m, const std::string& pfx) { m->param_ports = {{pfx + "/drive", 2}}; },
           nullptr, nullptr});
    // Triangular ADAA wavefolder — Buchla/Serge west-coast topology.
    // Hard reflections at ±1; multi-fold at high drive; output bounded ±1.
    // Inputs: in-l(0), in-r(1), drive(2, 0=unity, 1=16×).
    r.add("folder",
          {[]() -> std::unique_ptr<GridModule> {
               return std::make_unique<mi::WaveshaperModule>(&mi::WaveshaperModule::f_fold_tri,
                                                             &mi::WaveshaperModule::F_fold_tri);
           },
           [](GridModule* m, const std::string& pfx) { m->param_ports = {{pfx + "/drive", 2}}; },
           nullptr, nullptr});
    // stmlib rational saturation — from stmlib/dsp/dsp.h, wrapped with ADAA.
    // stml-soft-limit: smooth compression, unbounded (grows as x/9 for large |x|).
    // stml-soft-clip:  SoftLimit for |x|≤3, hard clipped to ±1 beyond; C¹ at ±3.
    // Inputs: in-l(0), in-r(1), drive(2, 0=unity, 1=16×).
    r.add("stml-soft-limit",
          {[]() -> std::unique_ptr<GridModule> {
               return std::make_unique<mi::WaveshaperModule>(&mi::WaveshaperModule::f_soft_limit,
                                                             &mi::WaveshaperModule::F_soft_limit);
           },
           [](GridModule* m, const std::string& pfx) { m->param_ports = {{pfx + "/drive", 2}}; },
           nullptr, nullptr});
    r.add("stml-soft-clip",
          {[]() -> std::unique_ptr<GridModule> {
               return std::make_unique<mi::WaveshaperModule>(&mi::WaveshaperModule::f_soft_clip,
                                                             &mi::WaveshaperModule::F_soft_clip);
           },
           [](GridModule* m, const std::string& pfx) { m->param_ports = {{pfx + "/drive", 2}}; },
           nullptr, nullptr});
}
