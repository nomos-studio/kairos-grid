// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/buffer/sah_module.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace kairos_grid;
using Catch::Approx;

namespace {

constexpr GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};

// Step one block: set all inputs, call process, return L output.
float step(SahModule& m, float in_l, float in_r = 0.f, float trig = 0.f, float rate = 0.f) {
    m.inputs[0].voltage = in_l;
    m.inputs[1].voltage = in_r;
    m.inputs[2].voltage = trig;
    m.inputs[3].voltage = rate;
    m.process(kArgs);
    return m.outputs[0].voltage;
}

} // namespace

TEST_CASE("SahModule: port counts", "[sah]") {
    SahModule m;
    REQUIRE(m.inputs.size() == 4);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("SahModule: default output is zero before any trigger", "[sah]") {
    SahModule m;
    m.prepare(kArgs);
    const float out = step(m, 0.9f); // trig=0, rate=0 → no latch
    REQUIRE(out == 0.f);
}

TEST_CASE("SahModule: rising edge of trig latches input", "[sah]") {
    SahModule m;
    m.prepare(kArgs);
    step(m, 0.5f, 0.f, 0.f);                   // trig low — no latch
    const float out = step(m, 0.5f, 0.f, 1.f); // trig rises → latch 0.5
    REQUIRE(out == Approx(0.5f));
}

TEST_CASE("SahModule: held value persists when input changes after latch", "[sah]") {
    SahModule m;
    m.prepare(kArgs);
    step(m, 0.7f, 0.f, 0.f); // pre-latch: trig low
    step(m, 0.7f, 0.f, 1.f); // rising edge: latch 0.7
    // Input changes, trig stays high (sustained — no second rising edge)
    const float out = step(m, 0.99f, 0.f, 1.f);
    REQUIRE(out == Approx(0.7f));
}

TEST_CASE("SahModule: second rising edge latches new value", "[sah]") {
    SahModule m;
    m.prepare(kArgs);
    step(m, 0.3f, 0.f, 0.f);                   // trig low
    step(m, 0.3f, 0.f, 1.f);                   // rising: latch 0.3
    step(m, 0.3f, 0.f, 1.f);                   // sustained: no second latch
    step(m, 0.3f, 0.f, 0.f);                   // trig falls
    const float out = step(m, 0.8f, 0.f, 1.f); // new rising: latch 0.8
    REQUIRE(out == Approx(0.8f));
}

TEST_CASE("SahModule: no latch on falling edge", "[sah]") {
    SahModule m;
    m.prepare(kArgs);
    step(m, 0.4f, 0.f, 0.f); // trig low
    step(m, 0.4f, 0.f, 1.f); // rising edge: latch 0.4
    // Trig falls — new input is 0.9 but falling edge must NOT re-latch.
    const float out = step(m, 0.9f, 0.f, 0.f); // falling: no latch, holds 0.4
    REQUIRE(out == Approx(0.4f));
}

TEST_CASE("SahModule: sustained high trig does not re-latch", "[sah]") {
    SahModule m;
    m.prepare(kArgs);
    step(m, 1.f, 0.f, 0.f);                    // trig low: no latch, in=1 not captured
    step(m, 1.f, 0.f, 1.f);                    // rising: latch 1.0
    const float out1 = step(m, 2.f, 0.f, 1.f); // sustained: no latch, still 1.0
    const float out2 = step(m, 3.f, 0.f, 1.f); // sustained: no latch, still 1.0
    REQUIRE(out1 == Approx(1.f));
    REQUIRE(out2 == Approx(1.f));
}

TEST_CASE("SahModule: stereo — L and R hold independently", "[sah]") {
    SahModule m;
    m.prepare(kArgs);
    step(m, 0.f, 0.f, 0.f);   // trig low
    step(m, 0.3f, 0.8f, 1.f); // rising: latch L=0.3, R=0.8
    REQUIRE(m.outputs[0].voltage == Approx(0.3f));
    REQUIRE(m.outputs[1].voltage == Approx(0.8f));
}

TEST_CASE("SahModule: rate>0 triggers periodic latch without external trig", "[sah]") {
    // rate = 2/4799 → period = 1 + 2 = 3.
    // Block 1: counter=1, no latch, out=0
    // Block 2: counter=2, no latch, out=0
    // Block 3: counter=3 >= 3 → latch, out=in at block 3
    SahModule m;
    m.prepare(kArgs);
    const float rate = 2.f / 4799.f;
    step(m, 1.f, 0.f, 0.f, rate);                   // block 1: counter=1, no latch
    step(m, 2.f, 0.f, 0.f, rate);                   // block 2: counter=2, no latch
    const float out = step(m, 3.f, 0.f, 0.f, rate); // block 3: latch → 3.0
    REQUIRE(out == Approx(3.f));
}

TEST_CASE("SahModule: rate=0 disables internal counter", "[sah]") {
    // With rate=0, no internal trigger fires regardless of block count.
    SahModule m;
    m.prepare(kArgs);
    for (int i = 0; i < 100; ++i)
        step(m, 0.9f, 0.f, 0.f, 0.f); // rate=0, trig=0 → never latches
    REQUIRE(m.outputs[0].voltage == 0.f);
}

TEST_CASE("SahModule: rate trigger and external trig can coexist", "[sah]") {
    // period=3 via rate; external trig fires on block 1 → latch earlier than internal.
    SahModule m;
    m.prepare(kArgs);
    const float rate = 2.f / 4799.f;
    // Block 1: external rising edge → latch 0.5 immediately (before counter would fire)
    step(m, 0.f, 0.f, 0.f, rate);                    // prime: trig low
    const float out = step(m, 0.5f, 0.f, 1.f, rate); // ext trigger fires: latch 0.5
    REQUIRE(out == Approx(0.5f));
}

TEST_CASE("SahModule: prepare resets held value", "[sah]") {
    SahModule m;
    m.prepare(kArgs);
    step(m, 0.f, 0.f, 0.f);
    step(m, 0.7f, 0.f, 1.f); // latch 0.7
    m.prepare(kArgs);        // reset
    const float out = step(m, 0.f, 0.f, 0.f);
    REQUIRE(out == 0.f);
}

TEST_CASE("SahModule: rate CV clamped — above-one acts like rate=1", "[sah]") {
    // rate=1.5 → clamped to 1.0 → period=4800; counter increments slowly.
    // After 1 block, counter=1, no latch yet.
    SahModule m;
    m.prepare(kArgs);
    const float out = step(m, 0.9f, 0.f, 0.f, 1.5f); // over-range rate
    REQUIRE(out == 0.f); // period=4800, counter=1 after block 1, no latch
}

TEST_CASE("SahModule: all outputs finite", "[sah]") {
    SahModule m;
    m.prepare(kArgs);
    for (int i = 0; i < 50; ++i) {
        const float v = static_cast<float>(i) / 25.f - 1.f;
        step(m, v, -v, (i % 7 == 0) ? 1.f : 0.f, 0.f);
        REQUIRE(std::isfinite(m.outputs[0].voltage));
        REQUIRE(std::isfinite(m.outputs[1].voltage));
    }
}
