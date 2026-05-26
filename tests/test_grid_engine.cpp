// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for GridModule, GridGraph, and GridEngine.

#include <kairos_grid/grid_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace kairos_grid;

// ---------------------------------------------------------------------------
// Stub modules for testing
// ---------------------------------------------------------------------------

// Emits a fixed voltage regardless of inputs.
class ConstModule : public GridModule {
  public:
    explicit ConstModule(float value) : GridModule(0, 1), value_(value) {}
    void process(const GridProcessArgs&) override { outputs[0].voltage = value_; }

  private:
    float value_;
};

// Multiplies its single input by a fixed gain.
class GainModule : public GridModule {
  public:
    explicit GainModule(float gain) : GridModule(1, 1), gain_(gain) {}
    void process(const GridProcessArgs&) override {
        outputs[0].voltage = inputs[0].voltage * gain_;
    }

  private:
    float gain_;
};

// Adds two inputs.
class SumModule : public GridModule {
  public:
    SumModule() : GridModule(2, 1) {}
    void process(const GridProcessArgs&) override {
        outputs[0].voltage = inputs[0].voltage + inputs[1].voltage;
    }
};

// Records the sample_rate seen on the first prepare() call.
class SampleRateCaptureModule : public GridModule {
  public:
    SampleRateCaptureModule() : GridModule(0, 0) {}
    void prepare(const GridProcessArgs& a) override { captured_rate = a.sample_rate; }
    void process(const GridProcessArgs&) override {}
    float captured_rate = 0.f;
};

// Records the frame counter seen in the most recent process() call.
class FrameCounterModule : public GridModule {
  public:
    FrameCounterModule() : GridModule(0, 0) {}
    void process(const GridProcessArgs& a) override { last_frame = a.frame; }
    int64_t last_frame = -1;
};

// ---------------------------------------------------------------------------
// GridGraph build tests
// ---------------------------------------------------------------------------

TEST_CASE("GridGraph builds a single module with no cables") {
    GridGraph g;
    g.add_module(std::make_unique<ConstModule>(1.f));
    auto res = g.build();
    REQUIRE(res.has_value());
    REQUIRE(res->module_count() == 1);
}

TEST_CASE("GridGraph builds a linear chain") {
    GridGraph g;
    int i_src  = g.add_module(std::make_unique<ConstModule>(1.f));
    int i_gain = g.add_module(std::make_unique<GainModule>(0.5f));
    g.add_cable({i_src, 0, i_gain, 0});

    auto res = g.build();
    REQUIRE(res.has_value());
    REQUIRE(res->module_count() == 2);
}

TEST_CASE("GridGraph detects a two-node cycle") {
    GridGraph g;
    int a = g.add_module(std::make_unique<GainModule>(1.f));
    int b = g.add_module(std::make_unique<GainModule>(1.f));
    g.add_cable({a, 0, b, 0});
    g.add_cable({b, 0, a, 0});

    auto res = g.build();
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error() == graph_error::cycle_detected);
}

TEST_CASE("GridGraph rejects invalid module index in cable") {
    GridGraph g;
    g.add_module(std::make_unique<ConstModule>(0.f)); // index 0
    g.add_cable({0, 0, 99, 0});                       // module 99 doesn't exist

    auto res = g.build();
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error() == graph_error::invalid_module_index);
}

TEST_CASE("GridGraph rejects invalid port index in cable") {
    GridGraph g;
    int a = g.add_module(std::make_unique<ConstModule>(0.f)); // 0 inputs, 1 output
    int b = g.add_module(std::make_unique<GainModule>(1.f));  // 1 input,  1 output
    g.add_cable({a, 5, b, 0});                                // port 5 doesn't exist

    auto res = g.build();
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error() == graph_error::invalid_port_index);
}

TEST_CASE("GridGraph accepts a diamond topology") {
    // src → gain_a ↘
    //              sum → (check)
    // src → gain_b ↗
    GridGraph g;
    int i_src   = g.add_module(std::make_unique<ConstModule>(1.f));
    int i_gain_a = g.add_module(std::make_unique<GainModule>(2.f));
    int i_gain_b = g.add_module(std::make_unique<GainModule>(3.f));
    int i_sum   = g.add_module(std::make_unique<SumModule>());
    g.add_cable({i_src,    0, i_gain_a, 0});
    g.add_cable({i_src,    0, i_gain_b, 0});
    g.add_cable({i_gain_a, 0, i_sum,   0});
    g.add_cable({i_gain_b, 0, i_sum,   1});

    auto res = g.build();
    REQUIRE(res.has_value());
}

// ---------------------------------------------------------------------------
// GridEngine::prepare() tests
// ---------------------------------------------------------------------------

TEST_CASE("GridEngine::prepare propagates sample rate to all modules") {
    float captured = 0.f;

    // Use a raw pointer to observe the captured rate after build().
    auto cap = std::make_unique<SampleRateCaptureModule>();
    auto* cap_ptr = cap.get();

    GridGraph g;
    g.add_module(std::move(cap));
    auto res = g.build();
    REQUIRE(res.has_value());

    res->prepare(44100.f);
    REQUIRE_THAT(cap_ptr->captured_rate, Catch::Matchers::WithinAbs(44100.f, 1.f));
}

TEST_CASE("GridEngine::prepare resets frame counter") {
    auto fc = std::make_unique<FrameCounterModule>();
    auto* fc_ptr = fc.get();

    GridGraph g;
    g.add_module(std::move(fc));
    auto res = g.build();
    REQUIRE(res.has_value());

    res->prepare(48000.f);
    res->step_block(5);
    REQUIRE(fc_ptr->last_frame == 4); // 0-indexed: frames 0,1,2,3,4

    res->prepare(48000.f);            // reset
    res->step_block(1);
    REQUIRE(fc_ptr->last_frame == 0);
}

// ---------------------------------------------------------------------------
// GridEngine::step_block() signal flow tests
// ---------------------------------------------------------------------------

// VCVRack-style cable routing: output voltages are propagated AFTER all
// modules process() in a given sample. So in a chain A→B:
//   Sample 1: A computes outputs, B reads old inputs (0.f initially).
//             Cable routes A.out → B.in.
//   Sample 2: B reads the value A produced in sample 1.
// This is the canonical 1-sample cable latency per hop.

TEST_CASE("GridEngine: ConstModule output is set after one sample") {
    GridGraph g;
    int i_src = g.add_module(std::make_unique<ConstModule>(1.f));
    auto res = g.build();
    REQUIRE(res.has_value());

    res->prepare(48000.f);
    res->step_block(1);

    REQUIRE_THAT(res->module(i_src)->outputs[0].voltage,
                 Catch::Matchers::WithinAbs(1.f, 1e-6f));
}

TEST_CASE("GridEngine: GainModule sees correct input after 2 samples (1-sample cable latency)") {
    GridGraph g;
    int i_src  = g.add_module(std::make_unique<ConstModule>(1.f));
    int i_gain = g.add_module(std::make_unique<GainModule>(0.5f));
    g.add_cable({i_src, 0, i_gain, 0});

    auto res = g.build();
    REQUIRE(res.has_value());

    res->prepare(48000.f);
    // Sample 1: ConstModule sets output=1.0; cable routes to GainModule.input.
    //           GainModule still reads 0.0 → output=0.0.
    // Sample 2: GainModule reads 1.0 → output=0.5.
    res->step_block(2);

    REQUIRE_THAT(res->module(i_gain)->outputs[0].voltage,
                 Catch::Matchers::WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("GridEngine: diamond graph sums correctly after propagation") {
    // src(1.0) → gain_a(×2) ↘
    //                         sum → 2.0 + 3.0 = 5.0
    // src(1.0) → gain_b(×3) ↗
    // (src fans out to both gains via 2 cables)
    GridGraph g;
    int i_src    = g.add_module(std::make_unique<ConstModule>(1.f));
    int i_gain_a = g.add_module(std::make_unique<GainModule>(2.f));
    int i_gain_b = g.add_module(std::make_unique<GainModule>(3.f));
    int i_sum    = g.add_module(std::make_unique<SumModule>());
    g.add_cable({i_src,    0, i_gain_a, 0});
    g.add_cable({i_src,    0, i_gain_b, 0});
    g.add_cable({i_gain_a, 0, i_sum,   0});
    g.add_cable({i_gain_b, 0, i_sum,   1});

    auto res = g.build();
    REQUIRE(res.has_value());

    res->prepare(48000.f);
    res->step_block(3); // 3 hops: src→gain_a/b (1), gain_a/b→sum (2), sum reads (3)

    REQUIRE_THAT(res->module(i_sum)->outputs[0].voltage,
                 Catch::Matchers::WithinAbs(5.f, 1e-6f));
}

TEST_CASE("GridEngine: step_block processes multiple blocks correctly") {
    GridGraph g;
    int i_src = g.add_module(std::make_unique<ConstModule>(1.f));
    auto res = g.build();
    REQUIRE(res.has_value());

    res->prepare(48000.f);
    res->step_block(64);
    res->step_block(128);
    res->step_block(256);

    // ConstModule output is always 1.f regardless of frame count.
    REQUIRE_THAT(res->module(i_src)->outputs[0].voltage,
                 Catch::Matchers::WithinAbs(1.f, 1e-6f));
}

TEST_CASE("GridEngine: frame counter increments correctly") {
    auto fc = std::make_unique<FrameCounterModule>();
    auto* fc_ptr = fc.get();

    GridGraph g;
    g.add_module(std::move(fc));
    auto res = g.build();
    REQUIRE(res.has_value());

    res->prepare(48000.f);
    res->step_block(10);
    REQUIRE(fc_ptr->last_frame == 9); // processed frames 0..9
    res->step_block(5);
    REQUIRE(fc_ptr->last_frame == 14); // continued: frames 10..14
}

TEST_CASE("GridEngine::module returns nullptr for out-of-range index") {
    GridGraph g;
    g.add_module(std::make_unique<ConstModule>(0.f));
    auto res = g.build();
    REQUIRE(res.has_value());

    REQUIRE(res->module(0) != nullptr);
    REQUIRE(res->module(1) == nullptr);
    REQUIRE(res->module(-1) == nullptr);
}

// ---------------------------------------------------------------------------
// Stub modules for tap / param-bus tests
// ---------------------------------------------------------------------------

// Declares one performance tap ("signal/level") and one named param input
// ("control/gain"). Multiplies the named input by a fixed factor and writes
// the result to the tap.
class TapParamModule : public GridModule {
  public:
    explicit TapParamModule(float factor = 1.f)
        : GridModule(1, 1), factor_(factor)
    {
        param_ports.push_back({"control/gain", 0});
        taps.push_back({"signal/level", 0.f});
    }

    void process(const GridProcessArgs&) override {
        const float v       = inputs[0].voltage * factor_;
        outputs[0].voltage  = v;
        taps[0].value       = v;
    }

  private:
    float factor_;
};

// Declares two taps with different names.
class DualTapModule : public GridModule {
  public:
    DualTapModule() : GridModule(0, 0) {
        taps.push_back({"signal/alpha", 0.f});
        taps.push_back({"signal/beta",  0.f});
        alpha_val = 0.f;
        beta_val  = 0.f;
    }

    void process(const GridProcessArgs&) override {
        taps[0].value = alpha_val;
        taps[1].value = beta_val;
    }

    float alpha_val;
    float beta_val;
};

// ---------------------------------------------------------------------------
// TapSchema / PortSchema discovery tests
// ---------------------------------------------------------------------------

TEST_CASE("tap_schema is empty for engine with no taps") {
    GridGraph g;
    g.add_module(std::make_unique<ConstModule>(1.f));
    auto res = g.build();
    REQUIRE(res.has_value());

    res->prepare(48000.f);
    REQUIRE(res->tap_schema().size() == 0);
    REQUIRE(res->tap_schema().entries.empty());
}

TEST_CASE("port_schema is empty for engine with no named param ports") {
    GridGraph g;
    g.add_module(std::make_unique<ConstModule>(1.f));
    auto res = g.build();
    REQUIRE(res.has_value());

    res->prepare(48000.f);
    REQUIRE(res->port_schema().size() == 0);
}

TEST_CASE("tap_schema discovers one tap with correct name and id") {
    GridGraph g;
    g.add_module(std::make_unique<TapParamModule>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    const auto& ts = res->tap_schema();
    REQUIRE(ts.size() == 1);
    REQUIRE(ts.entries[0].id == 0);
    REQUIRE(ts.entries[0].name == "signal/level");
    REQUIRE(ts.entries[0].module_idx == 0);
    REQUIRE(ts.entries[0].tap_idx == 0);
}

TEST_CASE("port_schema discovers one param port with correct name") {
    GridGraph g;
    g.add_module(std::make_unique<TapParamModule>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    const auto& ps = res->port_schema();
    REQUIRE(ps.size() == 1);
    REQUIRE(ps.entries[0].id == 0);
    REQUIRE(ps.entries[0].name == "control/gain");
    REQUIRE(ps.entries[0].port_idx == 0);
}

TEST_CASE("tap_schema discovers two taps from a dual-tap module") {
    GridGraph g;
    g.add_module(std::make_unique<DualTapModule>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    const auto& ts = res->tap_schema();
    REQUIRE(ts.size() == 2);
    REQUIRE(ts.entries[0].name == "signal/alpha");
    REQUIRE(ts.entries[1].name == "signal/beta");
    REQUIRE(ts.entries[0].id == 0);
    REQUIRE(ts.entries[1].id == 1);
}

TEST_CASE("tap_schema aggregates taps across multiple modules") {
    GridGraph g;
    g.add_module(std::make_unique<TapParamModule>());  // 1 tap
    g.add_module(std::make_unique<DualTapModule>());   // 2 taps
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    REQUIRE(res->tap_schema().size() == 3);
    REQUIRE(res->port_schema().size() == 1); // only TapParamModule has a param port
}

// ---------------------------------------------------------------------------
// Schema epoch tests
// ---------------------------------------------------------------------------

TEST_CASE("tap_schema epoch increments on each prepare()") {
    GridGraph g;
    g.add_module(std::make_unique<TapParamModule>());
    auto res = g.build();
    REQUIRE(res.has_value());

    res->prepare(48000.f);
    uint32_t epoch1 = res->tap_schema().epoch;

    res->prepare(48000.f);
    uint32_t epoch2 = res->tap_schema().epoch;

    REQUIRE(epoch2 == epoch1 + 1);
}

TEST_CASE("tap_schema and port_schema share the same epoch after prepare()") {
    GridGraph g;
    g.add_module(std::make_unique<TapParamModule>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    REQUIRE(res->tap_schema().epoch == res->port_schema().epoch);
}

// ---------------------------------------------------------------------------
// tap_frame() harvest tests
// ---------------------------------------------------------------------------

TEST_CASE("tap_frame is zero-initialised before first step_block") {
    GridGraph g;
    g.add_module(std::make_unique<TapParamModule>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // tap_frame built by prepare(); modules haven't run yet.
    const auto tf = res->tap_frame();
    REQUIRE(tf.size() == 1);
    REQUIRE_THAT(tf[0], Catch::Matchers::WithinAbs(0.f, 1e-9f));
}

TEST_CASE("tap_frame reflects tap value written during process()") {
    auto* raw = new TapParamModule(1.f);
    auto  ptr = std::unique_ptr<TapParamModule>(raw);

    GridGraph g;
    g.add_module(std::move(ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // Drive the input directly (no named param needed here).
    raw->inputs[0].voltage = 3.f;
    res->step_block(1);

    const auto tf = res->tap_frame();
    REQUIRE(tf.size() == 1);
    REQUIRE_THAT(tf[0], Catch::Matchers::WithinAbs(3.f, 1e-6f));
}

TEST_CASE("tap_frame size matches tap_schema size") {
    GridGraph g;
    g.add_module(std::make_unique<DualTapModule>());
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    REQUIRE(res->tap_frame().size() ==
            static_cast<std::size_t>(res->tap_schema().size()));
}

TEST_CASE("tap_frame tracks two taps independently") {
    auto* raw = new DualTapModule;
    auto  ptr = std::unique_ptr<DualTapModule>(raw);

    GridGraph g;
    g.add_module(std::move(ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    raw->alpha_val = 0.25f;
    raw->beta_val  = 0.75f;
    res->step_block(1);

    const auto tf = res->tap_frame();
    REQUIRE_THAT(tf[0], Catch::Matchers::WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(tf[1], Catch::Matchers::WithinAbs(0.75f, 1e-6f));
}

// ---------------------------------------------------------------------------
// apply_params() tests
// ---------------------------------------------------------------------------

TEST_CASE("apply_params writes named input port voltage") {
    auto* raw = new TapParamModule(1.f);
    auto  ptr = std::unique_ptr<TapParamModule>(raw);

    GridGraph g;
    g.add_module(std::move(ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    const std::array<float, 1> frame = {2.5f};
    res->apply_params(frame);

    REQUIRE_THAT(raw->inputs[0].voltage, Catch::Matchers::WithinAbs(2.5f, 1e-6f));
}

TEST_CASE("apply_params + step_block round-trip to tap_frame") {
    GridGraph g;
    g.add_module(std::make_unique<TapParamModule>(2.f));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // apply_params sets input to 1.5; TapParamModule multiplies by factor 2 → tap = 3.0
    const std::array<float, 1> frame = {1.5f};
    res->apply_params(frame);
    res->step_block(1);

    REQUIRE_THAT(res->tap_frame()[0], Catch::Matchers::WithinAbs(3.f, 1e-6f));
}

TEST_CASE("apply_params with empty span is a no-op") {
    auto* raw = new TapParamModule(1.f);
    auto  ptr = std::unique_ptr<TapParamModule>(raw);
    raw->inputs[0].voltage = 7.f;  // pre-set

    GridGraph g;
    g.add_module(std::move(ptr));
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    res->apply_params({});  // empty span — should not clear the port

    // Input unchanged from the value we set before build.
    // Note: prepare() doesn't zero ports, so the pre-set value survives.
    REQUIRE_THAT(raw->inputs[0].voltage, Catch::Matchers::WithinAbs(7.f, 1e-6f));
}

TEST_CASE("apply_params ignores extra frame entries beyond port_schema size") {
    GridGraph g;
    g.add_module(std::make_unique<TapParamModule>());  // 1 named port
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // 3 values, only 1 named port — should not crash, only first value applied.
    const std::array<float, 3> frame = {4.f, 5.f, 6.f};
    REQUIRE_NOTHROW(res->apply_params(frame));
    REQUIRE(res->port_schema().size() == 1);
}
