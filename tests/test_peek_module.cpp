// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/buffer/peek_module.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace kairos_grid;
using Catch::Approx;

namespace {

constexpr GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};

// Drive L and (optionally) R index inputs, call process, return L output.
float step(PeekModule& m, float idx_l, float idx_r = -1.f) {
    if (idx_r < 0.f)
        idx_r = idx_l;
    m.inputs[0].voltage = idx_l;
    m.inputs[1].voltage = idx_r;
    m.process(kArgs);
    return m.outputs[0].voltage;
}

} // namespace

TEST_CASE("PeekModule: port counts", "[peek]") {
    SECTION("linear") {
        PeekModule m(PeekInterp::Linear);
        REQUIRE(m.inputs.size() == 2);
        REQUIRE(m.outputs.size() == 2);
    }
    SECTION("nearest-neighbor") {
        PeekModule m(PeekInterp::None);
        REQUIRE(m.inputs.size() == 2);
        REQUIRE(m.outputs.size() == 2);
    }
    SECTION("cubic") {
        PeekModule m(PeekInterp::Cubic);
        REQUIRE(m.inputs.size() == 2);
        REQUIRE(m.outputs.size() == 2);
    }
}

TEST_CASE("PeekModule: blank buffer reads zero", "[peek]") {
    PeekModule m(PeekInterp::Linear);
    m.inputs[0].voltage = 0.5f;
    m.inputs[1].voltage = 0.5f;
    m.process(kArgs);
    REQUIRE(m.outputs[0].voltage == 0.f);
    REQUIRE(m.outputs[1].voltage == 0.f);
}

TEST_CASE("PeekModule: index=0 reads first sample", "[peek]") {
    PeekModule m(PeekInterp::Linear, 8);
    for (std::size_t i = 0; i < 8; ++i)
        m.buffer()[i] = static_cast<float>(i) + 1.f; // 1, 2, 3, 4, 5, 6, 7, 8

    REQUIRE(step(m, 0.f) == Approx(1.f));
}

TEST_CASE("PeekModule: index=1 reads last sample", "[peek]") {
    PeekModule m(PeekInterp::Linear, 8);
    for (std::size_t i = 0; i < 8; ++i)
        m.buffer()[i] = static_cast<float>(i) + 1.f; // 1..8; last = 8

    REQUIRE(step(m, 1.f) == Approx(8.f));
}

TEST_CASE("PeekModule: linear interpolation at midpoint", "[peek]") {
    // buffer = {0, 10, 20, 30}, N=4.
    // index = 0.5/3 → pos_f = 0.5 → lerp(buffer[0]=0, buffer[1]=10, 0.5) = 5.
    PeekModule m(PeekInterp::Linear, 4);
    m.buffer() = {0.f, 10.f, 20.f, 30.f};

    REQUIRE(step(m, 0.5f / 3.f) == Approx(5.f).margin(1e-4f));
}

TEST_CASE("PeekModule: nearest-neighbor uses floor (staircase)", "[peek]") {
    // buffer = {0, 1, 2, 3}, N=4.  pos_f = idx * (N-1) = idx * 3.
    // floor(pos_f) determines which sample is returned.
    PeekModule m(PeekInterp::None, 4);
    m.buffer() = {0.f, 1.f, 2.f, 3.f};

    REQUIRE(step(m, 0.f) == Approx(0.f));   // pos=0.0  → buf[0]
    REQUIRE(step(m, 0.3f) == Approx(0.f));  // pos=0.9  → floor=0 → buf[0]
    REQUIRE(step(m, 0.34f) == Approx(1.f)); // pos=1.02 → floor=1 → buf[1]
    REQUIRE(step(m, 0.66f) == Approx(1.f)); // pos=1.98 → floor=1 → buf[1]
    REQUIRE(step(m, 0.67f) == Approx(2.f)); // pos=2.01 → floor=2 → buf[2]
    REQUIRE(step(m, 1.f) == Approx(3.f));   // pos=3.0  → buf[3]
}

TEST_CASE("PeekModule: cubic at exact integer positions is exact", "[peek]") {
    // Catmull-Rom at f=0 collapses to d = y1 exactly.
    // buffer = {0, 1, 4, 9, 16, 25, 36, 49}, N=8.
    // index = 3/7 → pos_f = 3.0 exactly → cubic returns buffer[3] = 9.
    PeekModule m(PeekInterp::Cubic, 8);
    for (std::size_t i = 0; i < 8; ++i)
        m.buffer()[i] = static_cast<float>(i * i);

    REQUIRE(step(m, 3.f / 7.f) == Approx(9.f).margin(1e-4f));
}

TEST_CASE("PeekModule: stereo channels read independently", "[peek]") {
    // L at index=0 reads first sample; R at index=1 reads last sample.
    PeekModule m(PeekInterp::Linear, 8);
    for (std::size_t i = 0; i < 8; ++i)
        m.buffer()[i] = static_cast<float>(i); // 0..7

    m.inputs[0].voltage = 0.f;
    m.inputs[1].voltage = 1.f;
    m.process(kArgs);
    REQUIRE(m.outputs[0].voltage == Approx(0.f));
    REQUIRE(m.outputs[1].voltage == Approx(7.f));
}

TEST_CASE("PeekModule: index clamping — below zero and above one", "[peek]") {
    PeekModule m(PeekInterp::Linear, 4);
    m.buffer() = {10.f, 20.f, 30.f, 40.f};

    // below zero → clamp to 0 → same result as index=0
    REQUIRE(step(m, -0.5f) == Approx(step(m, 0.f)));

    // above one → clamp to 1 → same result as index=1
    REQUIRE(step(m, 1.5f) == Approx(step(m, 1.f)));
}

TEST_CASE("PeekModule: fill_sine produces correct sine values", "[peek]") {
    // N=8.  buffer[i] = sin(2π * i / N).
    // Checkpoints: i=0→0, i=2→sin(π/2)=1, i=4→sin(π)=0, i=6→sin(3π/2)=-1.
    std::vector<float> buf(8);
    PeekModule::fill_sine(buf);

    REQUIRE(buf[0] == Approx(0.f).margin(1e-6f));
    REQUIRE(buf[2] == Approx(1.f).margin(1e-6f));
    REQUIRE(buf[4] == Approx(0.f).margin(1e-6f));
    REQUIRE(buf[6] == Approx(-1.f).margin(1e-6f));
}

TEST_CASE("PeekModule: fill_triangle produces correct triangle values", "[peek]") {
    // N=8.  t = i/8: 0→0, 0.25→1, 0.5→0, 0.75→-1.
    // buf[2]: t=0.25 → 1;  buf[6]: t=0.75 → -1.
    std::vector<float> buf(8);
    PeekModule::fill_triangle(buf);

    REQUIRE(buf[0] == Approx(0.f).margin(1e-5f));
    REQUIRE(buf[2] == Approx(1.f).margin(1e-5f));
    REQUIRE(buf[4] == Approx(0.f).margin(1e-5f));
    REQUIRE(buf[6] == Approx(-1.f).margin(1e-5f));
}

TEST_CASE("PeekModule: fill_saw produces correct rising sawtooth values", "[peek]") {
    // N=4.  buf[i] = 2*(i/4) - 1: -1, -0.5, 0, 0.5.
    std::vector<float> buf(4);
    PeekModule::fill_saw(buf);

    REQUIRE(buf[0] == Approx(-1.f).margin(1e-6f));
    REQUIRE(buf[1] == Approx(-0.5f).margin(1e-6f));
    REQUIRE(buf[2] == Approx(0.f).margin(1e-6f));
    REQUIRE(buf[3] == Approx(0.5f).margin(1e-6f));
}

TEST_CASE("PeekModule: linear and NN disagree between samples", "[peek]") {
    // Step-function buffer {0, 0, 1, 1}.  At midpoint between [1] and [2]:
    // NN floors to buffer[1]=0; linear interpolates to 0.5.
    const std::vector<float> data = {0.f, 0.f, 1.f, 1.f};

    PeekModule m_lin(PeekInterp::Linear, 4);
    m_lin.buffer() = data;
    PeekModule m_nn(PeekInterp::None, 4);
    m_nn.buffer() = data;

    // index=0.5 → pos_f = 0.5 * 3 = 1.5 → between buf[1]=0 and buf[2]=1.
    REQUIRE(step(m_lin, 0.5f) == Approx(0.5f).margin(1e-4f));
    REQUIRE(step(m_nn, 0.5f) == Approx(0.f));
}

TEST_CASE("PeekModule: all outputs finite on full index sweep (cubic, sine)", "[peek]") {
    PeekModule m(PeekInterp::Cubic, 32);
    PeekModule::fill_sine(m.buffer());

    for (int i = 0; i <= 100; ++i) {
        const float idx = static_cast<float>(i) / 100.f;
        REQUIRE(std::isfinite(step(m, idx)));
    }
}

TEST_CASE("PeekModule: fill_sine as wavetable stays within [-1, 1] on linear sweep", "[peek]") {
    PeekModule m(PeekInterp::Linear);
    PeekModule::fill_sine(m.buffer());

    for (int i = 0; i <= 200; ++i) {
        const float idx = static_cast<float>(i) / 200.f;
        const float out = step(m, idx);
        REQUIRE(out >= -1.001f);
        REQUIRE(out <= 1.001f);
    }
}
