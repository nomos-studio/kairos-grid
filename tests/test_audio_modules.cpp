// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for AudioInputModule and AudioOutputModule.

#include <kairos_grid/audio_modules.hpp>
#include <kairos_grid/grid_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <vector>

using namespace kairos_grid;

namespace {

static void prepare_at_48k(GridModule& m)
{
    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    m.prepare(args);
}

static void step_module(GridModule& m, int n)
{
    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    for (int i = 0; i < n; ++i) { args.frame = i; m.process(args); }
}

} // namespace

// ---------------------------------------------------------------------------
// AudioInputModule — structure
// ---------------------------------------------------------------------------

TEST_CASE("AudioInputModule: correct port counts") {
    AudioInputModule m;
    REQUIRE(m.inputs.size()  == 0);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("AudioInputModule: no taps or param ports") {
    AudioInputModule m;
    REQUIRE(m.taps.empty());
    REQUIRE(m.param_ports.empty());
}

// ---------------------------------------------------------------------------
// AudioInputModule — behaviour
// ---------------------------------------------------------------------------

TEST_CASE("AudioInputModule: outputs are zero before set_buffers") {
    AudioInputModule m;
    prepare_at_48k(m);
    step_module(m, 4);
    REQUIRE_THAT(m.outputs[AudioInputModule::k_left].voltage,
                 Catch::Matchers::WithinAbs(0.f, 1e-9f));
    REQUIRE_THAT(m.outputs[AudioInputModule::k_right].voltage,
                 Catch::Matchers::WithinAbs(0.f, 1e-9f));
}

TEST_CASE("AudioInputModule: reads left and right channels each sample") {
    AudioInputModule m;
    prepare_at_48k(m);

    std::array<float, 4> L = {0.1f, 0.2f, 0.3f, 0.4f};
    std::array<float, 4> R = {0.9f, 0.8f, 0.7f, 0.6f};
    float* bufs[2] = {L.data(), R.data()};
    m.set_buffers(bufs, 2, 4);

    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    for (int i = 0; i < 4; ++i) {
        args.frame = i;
        m.process(args);
        REQUIRE_THAT(m.outputs[AudioInputModule::k_left].voltage,
                     Catch::Matchers::WithinAbs(L[static_cast<std::size_t>(i)], 1e-6f));
        REQUIRE_THAT(m.outputs[AudioInputModule::k_right].voltage,
                     Catch::Matchers::WithinAbs(R[static_cast<std::size_t>(i)], 1e-6f));
    }
}

TEST_CASE("AudioInputModule: mono source — right channel is zero") {
    AudioInputModule m;
    prepare_at_48k(m);

    std::array<float, 2> L = {0.5f, 0.6f};
    float* bufs[1] = {L.data()};
    m.set_buffers(bufs, 1, 2);

    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    args.frame = 0; m.process(args);
    REQUIRE_THAT(m.outputs[AudioInputModule::k_left].voltage,
                 Catch::Matchers::WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(m.outputs[AudioInputModule::k_right].voltage,
                 Catch::Matchers::WithinAbs(0.f,  1e-9f));
}

TEST_CASE("AudioInputModule: outputs zero past end of buffer") {
    AudioInputModule m;
    prepare_at_48k(m);

    std::array<float, 2> L = {1.f, 1.f};
    std::array<float, 2> R = {1.f, 1.f};
    float* bufs[2] = {L.data(), R.data()};
    m.set_buffers(bufs, 2, 2);

    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    for (int i = 0; i < 2; ++i) { args.frame = i; m.process(args); }  // consume buffer
    args.frame = 2; m.process(args);  // one past end
    REQUIRE_THAT(m.outputs[AudioInputModule::k_left].voltage,
                 Catch::Matchers::WithinAbs(0.f, 1e-9f));
}

TEST_CASE("AudioInputModule: prepare resets state so next set_buffers works") {
    AudioInputModule m;
    prepare_at_48k(m);

    std::array<float, 1> L = {0.42f};
    std::array<float, 1> R = {0.84f};
    float* bufs[2] = {L.data(), R.data()};

    m.set_buffers(bufs, 2, 1);
    step_module(m, 1);  // consume

    prepare_at_48k(m);  // reset
    m.set_buffers(bufs, 2, 1);

    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    m.process(args);
    REQUIRE_THAT(m.outputs[AudioInputModule::k_left].voltage,
                 Catch::Matchers::WithinAbs(0.42f, 1e-6f));
}

// ---------------------------------------------------------------------------
// AudioOutputModule — structure
// ---------------------------------------------------------------------------

TEST_CASE("AudioOutputModule: correct port counts") {
    AudioOutputModule m;
    REQUIRE(m.inputs.size()  == 2);
    REQUIRE(m.outputs.size() == 0);
}

TEST_CASE("AudioOutputModule: no taps or param ports") {
    AudioOutputModule m;
    REQUIRE(m.taps.empty());
    REQUIRE(m.param_ports.empty());
}

// ---------------------------------------------------------------------------
// AudioOutputModule — behaviour
// ---------------------------------------------------------------------------

TEST_CASE("AudioOutputModule: writes L and R inputs to buffer each sample") {
    AudioOutputModule m;
    prepare_at_48k(m);

    std::array<float, 4> L{}, R{};
    float* bufs[2] = {L.data(), R.data()};
    m.set_buffers(bufs, 2, 4);

    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    const float vals[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    for (int i = 0; i < 4; ++i) {
        m.inputs[AudioOutputModule::k_left].voltage  = vals[i];
        m.inputs[AudioOutputModule::k_right].voltage = -vals[i];
        args.frame = i;
        m.process(args);
        REQUIRE_THAT(L[static_cast<std::size_t>(i)],
                     Catch::Matchers::WithinAbs( vals[i], 1e-6f));
        REQUIRE_THAT(R[static_cast<std::size_t>(i)],
                     Catch::Matchers::WithinAbs(-vals[i], 1e-6f));
    }
}

TEST_CASE("AudioOutputModule: extra output channels are zeroed") {
    AudioOutputModule m;
    prepare_at_48k(m);

    // 4-channel output buffer, module only fills 2.
    std::array<float, 2> ch0 = {0.f, 0.f};
    std::array<float, 2> ch1 = {0.f, 0.f};
    std::array<float, 2> ch2 = {9.f, 9.f};  // should be zeroed
    std::array<float, 2> ch3 = {9.f, 9.f};  // should be zeroed
    float* bufs[4] = {ch0.data(), ch1.data(), ch2.data(), ch3.data()};
    m.set_buffers(bufs, 4, 2);

    m.inputs[AudioOutputModule::k_left].voltage  = 1.f;
    m.inputs[AudioOutputModule::k_right].voltage = 1.f;
    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    m.process(args);

    REQUIRE_THAT(ch2[0], Catch::Matchers::WithinAbs(0.f, 1e-9f));
    REQUIRE_THAT(ch3[0], Catch::Matchers::WithinAbs(0.f, 1e-9f));
}

TEST_CASE("AudioOutputModule: no-op when set_buffers not called") {
    AudioOutputModule m;
    prepare_at_48k(m);
    m.inputs[AudioOutputModule::k_left].voltage  = 1.f;
    m.inputs[AudioOutputModule::k_right].voltage = 1.f;
    // No set_buffers call — must not crash or write anything.
    step_module(m, 4);
    // If we reach here without a crash, the test passes.
    REQUIRE(true);
}

TEST_CASE("AudioOutputModule: does not write past end of buffer") {
    AudioOutputModule m;
    prepare_at_48k(m);

    std::array<float, 2> L = {0.f, 0.f};
    std::array<float, 2> R = {0.f, 0.f};
    float* bufs[2] = {L.data(), R.data()};
    m.set_buffers(bufs, 2, 2);

    m.inputs[0].voltage = 1.f;
    m.inputs[1].voltage = 1.f;
    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    for (int i = 0; i < 4; ++i) { args.frame = i; m.process(args); }  // 2 extra calls
    // Buffers should only have values for first 2 samples — no OOB write.
    REQUIRE(L[0] == 1.f);
    REQUIRE(L[1] == 1.f);
}

// ---------------------------------------------------------------------------
// Integration — AudioInput → AudioOutput via GridEngine
// ---------------------------------------------------------------------------

TEST_CASE("AudioInput → AudioOutput: stereo pass-through via engine (1-sample latency)") {
    // GridEngine routes cables after each sample's process() calls, so
    // AudioOutputModule sees AudioInputModule's value one sample later.
    // dst[i] == src[i-1] for i >= 1; dst[0] == 0 (initial input voltage).
    GridGraph g;
    auto in_up  = std::make_unique<AudioInputModule>();
    auto out_up = std::make_unique<AudioOutputModule>();
    AudioInputModule*  raw_in  = in_up.get();
    AudioOutputModule* raw_out = out_up.get();
    const int in_idx  = g.add_module(std::move(in_up));
    const int out_idx = g.add_module(std::move(out_up));
    g.add_cable({in_idx, AudioInputModule::k_left,  out_idx, AudioOutputModule::k_left});
    g.add_cable({in_idx, AudioInputModule::k_right, out_idx, AudioOutputModule::k_right});

    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    constexpr int N = 8;
    std::array<float, N> src_L, src_R, dst_L{}, dst_R{};
    for (int i = 0; i < N; ++i) { src_L[i] = static_cast<float>(i + 1) * 0.1f; src_R[i] = -src_L[i]; }

    float* in_bufs[2]  = {src_L.data(), src_R.data()};
    float* out_bufs[2] = {dst_L.data(), dst_R.data()};
    raw_in->set_buffers(in_bufs, 2, N);
    raw_out->set_buffers(out_bufs, 2, N);

    res->step_block(N);

    // dst[0] = 0 (initial); dst[i] = src[i-1] for i >= 1.
    REQUIRE_THAT(dst_L[0], Catch::Matchers::WithinAbs(0.f, 1e-9f));
    for (int i = 1; i < N; ++i) {
        INFO("sample " << i);
        REQUIRE_THAT(dst_L[static_cast<std::size_t>(i)],
                     Catch::Matchers::WithinAbs(src_L[static_cast<std::size_t>(i - 1)], 1e-6f));
        REQUIRE_THAT(dst_R[static_cast<std::size_t>(i)],
                     Catch::Matchers::WithinAbs(src_R[static_cast<std::size_t>(i - 1)], 1e-6f));
    }
}

TEST_CASE("AudioInput → AudioOutput: cable latency is one sample") {
    // GridEngine routes cables after each sample's process() calls, so the
    // output module sees the input module's value one sample later.
    GridGraph g;
    auto in_up  = std::make_unique<AudioInputModule>();
    auto out_up = std::make_unique<AudioOutputModule>();
    AudioInputModule*  raw_in  = in_up.get();
    AudioOutputModule* raw_out = out_up.get();
    const int in_idx  = g.add_module(std::move(in_up));
    const int out_idx = g.add_module(std::move(out_up));
    g.add_cable({in_idx, AudioInputModule::k_left,  out_idx, AudioOutputModule::k_left});
    g.add_cable({in_idx, AudioInputModule::k_right, out_idx, AudioOutputModule::k_right});
    auto res = g.build();
    REQUIRE(res.has_value());
    res->prepare(48000.f);

    // Feed a single impulse at sample 0 and observe it at sample 1 in output.
    constexpr int N = 4;
    std::array<float, N> src_L = {1.f, 0.f, 0.f, 0.f};
    std::array<float, N> src_R = {0.f, 0.f, 0.f, 0.f};
    std::array<float, N> dst_L{}, dst_R{};
    float* in_bufs[2]  = {src_L.data(), src_R.data()};
    float* out_bufs[2] = {dst_L.data(), dst_R.data()};
    raw_in->set_buffers(in_bufs, 2, N);
    raw_out->set_buffers(out_bufs, 2, N);
    res->step_block(N);

    // Cable introduces one sample of latency.
    REQUIRE_THAT(dst_L[0], Catch::Matchers::WithinAbs(0.f, 1e-9f));
    REQUIRE_THAT(dst_L[1], Catch::Matchers::WithinAbs(1.f, 1e-6f));
    REQUIRE_THAT(dst_L[2], Catch::Matchers::WithinAbs(0.f, 1e-9f));
}
