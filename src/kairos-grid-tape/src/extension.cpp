// SPDX-License-Identifier: GPL-3.0-or-later
// kairos-grid-tape.kgext — ChowTape DSP module registrations.
// Modules: "hysteresis", "wow-flutter", "loss", "tape-comp", "tape".
#include <kairos_grid/grid_extension.hpp>

#include "Processors/Compression/CompressionProcessor.h"
#include "Processors/Hysteresis/HysteresisProcessing.h"
#include "Processors/Loss_Effects/LossFilter.h"
#include "Processors/Timing_Effects/WowFlutterProcessor.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

using namespace kairos_grid;

// ---------------------------------------------------------------------------
// Block-processing helper
//
// ChowTape processors work on AudioBuffer blocks. GridModule::process() is
// called once per sample. BlockWrapper<T> buffers kBlockSize samples, calls
// T::processBlock() at block boundaries, and drains the processed output.
// Latency: exactly kBlockSize samples.
// ---------------------------------------------------------------------------
static constexpr int kBlockSize = 128;

template <typename T> struct BlockWrapper {
    T                  proc;
    AudioBuffer<float> buf; // stereo, kBlockSize; input written then processed in-place
    int                phase = kBlockSize; // start full so prepare() runs before first block

    void reset(int numChannels) {
        buf.setSize(numChannels, kBlockSize);
        buf.clear();
        phase = 0;
    }

    // Write one stereo input sample, update phase, return whether a block
    // just completed (caller should call processBlock if true).
    bool push(float inL, float inR) noexcept {
        buf.getWritePointer(0)[phase] = inL;
        if (buf.getNumChannels() > 1)
            buf.getWritePointer(1)[phase] = inR;
        ++phase;
        return phase == kBlockSize;
    }

    // Read one stereo output sample (position relative to last block boundary).
    // Caller must not call before the first push completes a block.
    float outL(int pos) const noexcept { return buf.getReadPointer(0)[pos]; }
    float outR(int pos) const noexcept {
        return (buf.getNumChannels() > 1) ? buf.getReadPointer(1)[pos] : 0.0f;
    }
};

// ---------------------------------------------------------------------------
// 1. TapeHysteresisModule — "hysteresis"
//
// Ports in:  in-l(0), in-r(1), drive(2), saturation(3), width(4), bias(5)
// Ports out: out-l(0), out-r(1)
// Processing: per-sample RK4 Jiles-Atherton, stereo.
// ---------------------------------------------------------------------------
class TapeHysteresisModule : public GridModule {
  public:
    TapeHysteresisModule() : GridModule(6, 2) {}

    void prepare(const GridProcessArgs& args) override {
        for (auto& h : hyst_)
            h.setSampleRate(args.sample_rate);
        cookFromPorts();
    }

    void process(const GridProcessArgs& /*args*/) override {
        cookFromPorts();

        auto H_L = static_cast<double>(inputs[0].voltage);
        auto H_R = static_cast<double>(inputs[1].voltage);

        auto M_L = hyst_[0].process<RK4>(H_L);
        auto M_R = hyst_[1].process<RK4>(H_R);

        // Normalise to ±1 (M_s ≈ 0.5 + 1.5*(1-sat))
        auto M_s           = 0.5 + 1.5 * (1.0 - std::clamp((double)inputs[3].voltage, 0.0, 1.0));
        outputs[0].voltage = static_cast<float>(M_L / M_s);
        outputs[1].voltage = static_cast<float>(M_R / M_s);
    }

  private:
    HysteresisProcessing hyst_[2];

    void cookFromPorts() {
        auto drive = std::clamp((double)inputs[2].voltage, 0.0, 1.0);
        auto sat   = std::clamp((double)inputs[3].voltage, 0.0, 1.0);
        auto width = std::clamp((double)inputs[4].voltage, 0.0, 1.0);
        bool v1    = inputs[5].voltage > 0.5f;
        for (auto& h : hyst_)
            h.cook(drive, width, sat, v1);
    }
};

// ---------------------------------------------------------------------------
// 2. TapeWowFlutterModule — "wow-flutter"
//
// Ports in:  in-l(0), in-r(1), flutter-rate(2), flutter-depth(3),
//            wow-rate(4), wow-depth(5), wow-variance(6), wow-drift(7)
// Ports out: out-l(0), out-r(1)
// ---------------------------------------------------------------------------
class TapeWowFlutterModule : public GridModule {
  public:
    TapeWowFlutterModule() : GridModule(8, 2) {}

    void prepare(const GridProcessArgs& args) override {
        wf_.reset(2);
        wf_.proc.prepareToPlay(args.sample_rate, kBlockSize, 2);
        outPhase_ = 0;
    }

    void process(const GridProcessArgs& /*args*/) override {
        // Read output from previous block
        if (wf_.phase > 0) {
            outputs[0].voltage = wf_.outL(wf_.phase - 1 < kBlockSize ? wf_.phase - 1 : 0);
            outputs[1].voltage = wf_.outR(wf_.phase - 1 < kBlockSize ? wf_.phase - 1 : 0);
        }

        if (wf_.push(inputs[0].voltage, inputs[1].voltage)) {
            wf_.proc.setParams(true, inputs[2].voltage, inputs[3].voltage, inputs[4].voltage,
                               inputs[5].voltage, inputs[6].voltage, inputs[7].voltage);
            wf_.proc.processBlock(wf_.buf);
            wf_.phase = 0;
        }

        outputs[0].voltage = wf_.outL(wf_.phase > 0 ? wf_.phase - 1 : 0);
        outputs[1].voltage = wf_.outR(wf_.phase > 0 ? wf_.phase - 1 : 0);
    }

  private:
    BlockWrapper<WowFlutterProcessor> wf_;
    int                               outPhase_ = 0;
};

// ---------------------------------------------------------------------------
// 3. TapeLossModule — "loss"
//
// Ports in:  in-l(0), in-r(1), speed(2), spacing(3), thickness(4),
//            gap(5), azimuth(6)
// Ports out: out-l(0), out-r(1)
// ---------------------------------------------------------------------------
class TapeLossModule : public GridModule {
  public:
    TapeLossModule() : GridModule(7, 2) {}

    void prepare(const GridProcessArgs& args) override {
        lf_.reset(2);
        lf_.proc.prepare((float)args.sample_rate, kBlockSize, 2);
    }

    void process(const GridProcessArgs& /*args*/) override {
        outputs[0].voltage = lf_.outL(lf_.phase > 0 ? lf_.phase - 1 : 0);
        outputs[1].voltage = lf_.outR(lf_.phase > 0 ? lf_.phase - 1 : 0);

        if (lf_.push(inputs[0].voltage, inputs[1].voltage)) {
            lf_.proc.setParams(true,
                               inputs[2].voltage,  // speed (ips)
                               inputs[3].voltage,  // spacing (µm)
                               inputs[4].voltage,  // thickness (µm)
                               inputs[5].voltage,  // gap (µm)
                               inputs[6].voltage); // azimuth (deg)
            lf_.proc.processBlock(lf_.buf);
            lf_.phase = 0;
        }
    }

  private:
    BlockWrapper<LossFilter> lf_;
};

// ---------------------------------------------------------------------------
// 4. TapeCompModule — "tape-comp"
//
// Ports in:  in-l(0), in-r(1), amount(2), attack(3), release(4)
// Ports out: out-l(0), out-r(1)
// ---------------------------------------------------------------------------
class TapeCompModule : public GridModule {
  public:
    TapeCompModule() : GridModule(5, 2) {}

    void prepare(const GridProcessArgs& args) override {
        cp_.reset(2);
        cp_.proc.prepare(args.sample_rate, kBlockSize, 2);
    }

    void process(const GridProcessArgs& /*args*/) override {
        outputs[0].voltage = cp_.outL(cp_.phase > 0 ? cp_.phase - 1 : 0);
        outputs[1].voltage = cp_.outR(cp_.phase > 0 ? cp_.phase - 1 : 0);

        if (cp_.push(inputs[0].voltage, inputs[1].voltage)) {
            cp_.proc.setParams(inputs[2].voltage > 0.0f, // on/off from amount being nonzero
                               inputs[2].voltage,        // amount (0–9 dB)
                               inputs[3].voltage,        // attack ms
                               inputs[4].voltage);       // release ms
            cp_.proc.processBlock(cp_.buf);
            cp_.phase = 0;
        }
    }

  private:
    BlockWrapper<CompressionProcessor> cp_;
};

// ---------------------------------------------------------------------------
// 5. TapeModule — "tape" composite
//
// Signal chain: hysteresis → lossFilter → wowFlutter (in-place, kBlockSize).
//
// Ports in:  in-l(0), in-r(1), drive(2), saturation(3), width(4),
//            speed(5), gap(6), flutter-depth(7), wow-depth(8)
// Ports out: out-l(0), out-r(1)
// ---------------------------------------------------------------------------
class TapeModule : public GridModule {
  public:
    TapeModule() : GridModule(9, 2) {}

    void prepare(const GridProcessArgs& args) override {
        buf_.setSize(2, kBlockSize);
        buf_.clear();

        for (auto& h : hyst_)
            h.setSampleRate(args.sample_rate);

        lossFilter_.prepare((float)args.sample_rate, kBlockSize, 2);
        wowFlutter_.prepareToPlay(args.sample_rate, kBlockSize, 2);

        phase_ = 0;
    }

    void process(const GridProcessArgs& /*args*/) override {
        // Output previously processed sample (one block latency)
        outputs[0].voltage = buf_.getReadPointer(0)[phase_];
        outputs[1].voltage = buf_.getReadPointer(1)[phase_];

        // Per-sample hysteresis (runs before block buffering)
        auto drive = std::clamp((double)inputs[2].voltage, 0.0, 1.0);
        auto sat   = std::clamp((double)inputs[3].voltage, 0.0, 1.0);
        auto width = std::clamp((double)inputs[4].voltage, 0.0, 1.0);
        auto M_s   = 0.5 + 1.5 * (1.0 - sat);
        hyst_[0].cook(drive, width, sat, false);
        hyst_[1].cook(drive, width, sat, false);

        auto hl = hyst_[0].process<RK4>((double)inputs[0].voltage);
        auto hr = hyst_[1].process<RK4>((double)inputs[1].voltage);

        buf_.getWritePointer(0)[phase_] = (float)(hl / M_s);
        buf_.getWritePointer(1)[phase_] = (float)(hr / M_s);

        ++phase_;
        if (phase_ == kBlockSize) {
            float speed   = std::max(inputs[5].voltage, 1.0f); // ips, min 1
            float gap     = std::max(inputs[6].voltage, 0.1f); // µm, min 0.1
            float flutter = inputs[7].voltage;
            float wow     = inputs[8].voltage;

            lossFilter_.setParams(true, speed, 0.1f, 0.1f, gap, 0.0f);
            lossFilter_.processBlock(buf_);

            wowFlutter_.setParams(true, 0.3f, flutter, 0.25f, wow, 0.0f, 0.0f);
            wowFlutter_.processBlock(buf_);

            phase_ = 0;
        }
    }

  private:
    AudioBuffer<float>   buf_;
    int                  phase_ = 0;
    HysteresisProcessing hyst_[2];
    LossFilter           lossFilter_;
    WowFlutterProcessor  wowFlutter_;
};

// ---------------------------------------------------------------------------
// Extension entry point
// ---------------------------------------------------------------------------
extern "C" void kairos_grid_extension_entry(GridModuleRegistry& r) {
    r.add("hysteresis",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<TapeHysteresisModule>(); },
           [](GridModule* m, const std::string& pfx) {
               m->param_ports = {
                   {pfx + "/drive", 2},
                   {pfx + "/saturation", 3},
                   {pfx + "/width", 4},
                   {pfx + "/bias", 5},
               };
           },
           nullptr, nullptr});

    r.add("wow-flutter",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<TapeWowFlutterModule>(); },
           [](GridModule* m, const std::string& pfx) {
               m->param_ports = {
                   {pfx + "/flutter-rate", 2}, {pfx + "/flutter-depth", 3}, {pfx + "/wow-rate", 4},
                   {pfx + "/wow-depth", 5},    {pfx + "/wow-variance", 6},  {pfx + "/wow-drift", 7},
               };
           },
           nullptr, nullptr});

    r.add("loss",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<TapeLossModule>(); },
           [](GridModule* m, const std::string& pfx) {
               m->param_ports = {
                   {pfx + "/speed", 2}, {pfx + "/spacing", 3}, {pfx + "/thickness", 4},
                   {pfx + "/gap", 5},   {pfx + "/azimuth", 6},
               };
           },
           nullptr, nullptr});

    r.add("tape-comp",
          {[]() -> std::unique_ptr<GridModule> { return std::make_unique<TapeCompModule>(); },
           [](GridModule* m, const std::string& pfx) {
               m->param_ports = {
                   {pfx + "/amount", 2},
                   {pfx + "/attack", 3},
                   {pfx + "/release", 4},
               };
           },
           nullptr, nullptr});

    r.add("tape", {[]() -> std::unique_ptr<GridModule> { return std::make_unique<TapeModule>(); },
                   [](GridModule* m, const std::string& pfx) {
                       m->param_ports = {
                           {pfx + "/drive", 2},     {pfx + "/saturation", 3},
                           {pfx + "/width", 4},     {pfx + "/speed", 5},
                           {pfx + "/gap", 6},       {pfx + "/flutter-depth", 7},
                           {pfx + "/wow-depth", 8},
                       };
                   },
                   nullptr, nullptr});
}
