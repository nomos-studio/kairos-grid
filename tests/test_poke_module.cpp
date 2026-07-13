// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/buffer/poke_module.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace kairos_grid;
using Catch::Approx;

namespace {

constexpr GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};

// Drive all poke inputs and call process.  Returns L output.
float step(PokeModule& m, float in_l, float in_r, float write_pos, float read_pos, float gate) {
    m.inputs[0].voltage = in_l;
    m.inputs[1].voltage = in_r;
    m.inputs[2].voltage = write_pos;
    m.inputs[3].voltage = read_pos;
    m.inputs[4].voltage = gate;
    m.process(kArgs);
    return m.outputs[0].voltage;
}

} // namespace

TEST_CASE("PokeModule: port counts", "[poke]") {
    PokeModule m;
    REQUIRE(m.inputs.size() == 5);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("PokeModule: gate=0 never writes — blank buffer reads zero", "[poke]") {
    PokeModule m;
    // gate=0 → no write; blank buffer → read should return 0 everywhere
    const float out = step(m, 0.9f, 0.9f, 0.5f, 0.5f, 0.f);
    REQUIRE(out == 0.f);
}

TEST_CASE("PokeModule: gate=1 writes and same-block read reflects the write", "[poke]") {
    // N=4; write-pos=0.5 → pos_f=1.5 → floor → buffer[1].
    // Read at exact integer index 1: read-pos = 1/3 → pos_f=1.0 → buffer[1] exactly.
    // Write-before-read semantics: the output reflects this block's write.
    PokeModule  m(4);
    const float out = step(m, 0.7f, 0.f, 0.5f, 1.f / 3.f, 1.f);
    REQUIRE(out == Approx(0.7f).margin(1e-4f));
}

TEST_CASE("PokeModule: write persists after gate goes low", "[poke]") {
    PokeModule m(4);
    // First block: write 0.6 at pos=0
    step(m, 0.6f, 0.f, 0.f, 0.f, 1.f);
    // Second block: gate=0, read at pos=0 — should still return 0.6
    const float out = step(m, 0.f, 0.f, 0.f, 0.f, 0.f);
    REQUIRE(out == Approx(0.6f).margin(1e-4f));
}

TEST_CASE("PokeModule: write at different position than read", "[poke]") {
    // Write at pos=0 (index 0), read at pos=1 (index N-1).
    // N=4 → index 3. Buffer[3] was never written → stays 0.
    PokeModule  m(4);
    const float out = step(m, 0.5f, 0.f, 0.f, 1.f, 1.f);
    REQUIRE(out == Approx(0.f).margin(1e-4f));
}

TEST_CASE("PokeModule: stereo — L and R write to independent buffers", "[poke]") {
    PokeModule m(4);
    // Write 0.3 to L and 0.8 to R at pos=0; read at pos=0
    step(m, 0.3f, 0.8f, 0.f, 0.f, 1.f);
    REQUIRE(m.outputs[0].voltage == Approx(0.3f).margin(1e-4f)); // L
    REQUIRE(m.outputs[1].voltage == Approx(0.8f).margin(1e-4f)); // R
}

TEST_CASE("PokeModule: write overwrites previous value at same slot", "[poke]") {
    PokeModule m(4);
    step(m, 0.3f, 0.f, 0.f, 0.f, 1.f);                   // write 0.3 at pos=0
    const float out = step(m, 0.9f, 0.f, 0.f, 0.f, 1.f); // write 0.9 at pos=0, read same
    REQUIRE(out == Approx(0.9f).margin(1e-4f));
}

TEST_CASE("PokeModule: sweep write-pos fills buffer", "[poke]") {
    // Fill all 4 slots by sweeping write-pos through four steps, then read each.
    PokeModule m(4);
    // pos=0 → index 0: 0/(4-1)=0; pos=1/3 → index 1; pos=2/3 → index 2; pos=1 → index 3
    step(m, 10.f, 0.f, 0.f / 3.f, 0.f, 1.f);
    step(m, 20.f, 0.f, 1.f / 3.f, 0.f, 1.f);
    step(m, 30.f, 0.f, 2.f / 3.f, 0.f, 1.f);
    step(m, 40.f, 0.f, 3.f / 3.f, 0.f, 1.f);

    // Verify buffer_l contents
    REQUIRE(m.buffer_l()[0] == Approx(10.f));
    REQUIRE(m.buffer_l()[1] == Approx(20.f));
    REQUIRE(m.buffer_l()[2] == Approx(30.f));
    REQUIRE(m.buffer_l()[3] == Approx(40.f));
}

TEST_CASE("PokeModule: read-pos linear interpolation between written slots", "[poke]") {
    // Write 0 at pos=0 and 10 at pos=1 (indices 0 and 3 for N=4).
    // Read at pos=0.5 → pos_f = 1.5 → lerp(buf[1], buf[2]) = lerp(0, 0) = 0
    // Actually: write at 0 hits index 0, write at 1 hits index 3.
    // Indices 1 and 2 stay 0. Linear read at pos=0.5 is within the unwritten region.
    // Better test: write at 0 and 1/3 to set adjacent slots, then read midpoint.
    PokeModule m(4);
    step(m, 0.f, 0.f, 0.f / 3.f, 0.f, 1.f);  // buf[0] = 0
    step(m, 10.f, 0.f, 1.f / 3.f, 0.f, 1.f); // buf[1] = 10
    // Read at midpoint between index 0 and 1: pos = 0.5/3 → pos_f=0.5 → lerp(0,10,0.5)=5
    step(m, 0.f, 0.f, 0.f, 0.5f / 3.f, 0.f); // gate=0, just read
    REQUIRE(m.outputs[0].voltage == Approx(5.f).margin(1e-4f));
}

TEST_CASE("PokeModule: write-pos and read-pos clamped at boundaries", "[poke]") {
    PokeModule m(4);
    m.buffer_l() = {1.f, 2.f, 3.f, 4.f};
    m.buffer_r() = {1.f, 2.f, 3.f, 4.f};

    // read-pos < 0 → clamped to 0 → first sample
    step(m, 0.f, 0.f, 0.f, -0.5f, 0.f);
    REQUIRE(m.outputs[0].voltage == Approx(1.f));

    // read-pos > 1 → clamped to 1 → last sample
    step(m, 0.f, 0.f, 0.f, 1.5f, 0.f);
    REQUIRE(m.outputs[0].voltage == Approx(4.f));

    // write-pos > 1 → clamped to last slot; gate=1; then read at 1
    step(m, 99.f, 0.f, 1.5f, 1.f, 1.f);
    REQUIRE(m.buffer_l()[3] == Approx(99.f));
}

TEST_CASE("PokeModule: buffer_l and buffer_r accessors are independent", "[poke]") {
    PokeModule m(4);
    m.buffer_l()[0] = 5.f;
    m.buffer_r()[0] = 7.f;

    // Read both at pos=0 with gate=0 (no write)
    step(m, 0.f, 0.f, 0.f, 0.f, 0.f);
    REQUIRE(m.outputs[0].voltage == Approx(5.f));
    REQUIRE(m.outputs[1].voltage == Approx(7.f));
}

TEST_CASE("PokeModule: all outputs finite on gate-high sweep", "[poke]") {
    PokeModule m(32);
    for (int i = 0; i <= 50; ++i) {
        const float pos    = static_cast<float>(i) / 50.f;
        const float in_val = static_cast<float>(i) / 25.f - 1.f; // -1..1
        step(m, in_val, -in_val, pos, pos, 1.f);
        REQUIRE(std::isfinite(m.outputs[0].voltage));
        REQUIRE(std::isfinite(m.outputs[1].voltage));
    }
}
