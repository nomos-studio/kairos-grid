// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/buffer/buffer_module.hpp>
#include <kairos_grid/buffer/peek_module.hpp>
#include <kairos_grid/buffer/poke_module.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace kairos_grid;
using Catch::Approx;

namespace {

constexpr GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};

// Step one block on a peek module; returns L output.
float peek_step(PeekModule& m, float idx_l, float idx_r = 0.f) {
    m.inputs[0].voltage = idx_l;
    m.inputs[1].voltage = idx_r;
    m.process(kArgs);
    return m.outputs[0].voltage;
}

// Step one block on a poke module; returns L output.
float poke_step(PokeModule& m, float in_l, float in_r, float write_pos, float read_pos,
                float gate) {
    m.inputs[0].voltage = in_l;
    m.inputs[1].voltage = in_r;
    m.inputs[2].voltage = write_pos;
    m.inputs[3].voltage = read_pos;
    m.inputs[4].voltage = gate;
    m.process(kArgs);
    return m.outputs[0].voltage;
}

} // namespace

// ---------------------------------------------------------------------------
// Standalone BufferModule
// ---------------------------------------------------------------------------

TEST_CASE("BufferModule: port counts", "[buffer]") {
    BufferModule m;
    REQUIRE(m.inputs.size() == 0);
    REQUIRE(m.outputs.size() == 0);
}

TEST_CASE("BufferModule: standalone — default size 2048, zero-initialised", "[buffer]") {
    BufferModule m;
    REQUIRE(m.buffer_l().size() == 2048);
    REQUIRE(m.buffer_r().size() == 2048);
    REQUIRE(m.buffer_l()[0] == 0.f);
    REQUIRE(m.buffer_r()[0] == 0.f);
}

TEST_CASE("BufferModule: standalone — custom size", "[buffer]") {
    BufferModule m(512);
    REQUIRE(m.buffer_l().size() == 512);
    REQUIRE(m.buffer_r().size() == 512);
}

TEST_CASE("BufferModule: standalone — L and R are independent", "[buffer]") {
    BufferModule m(4);
    m.buffer_l()[0] = 1.f;
    m.buffer_r()[0] = 2.f;
    REQUIRE(m.buffer_l()[0] == 1.f);
    REQUIRE(m.buffer_r()[0] == 2.f);
    // Different storage
    REQUIRE(&m.buffer_l()[0] != &m.buffer_r()[0]);
}

TEST_CASE("BufferModule: process is a no-op", "[buffer]") {
    BufferModule m(4);
    m.buffer_l()[0] = 5.f;
    m.process(kArgs);
    REQUIRE(m.buffer_l()[0] == 5.f); // unchanged
}

TEST_CASE("BufferModule: fill_sine works via buffer_l ref", "[buffer]") {
    BufferModule m(8);
    PeekModule::fill_sine(m.buffer_l());
    REQUIRE(m.buffer_l()[0] == Approx(0.f).margin(1e-6f));
    // Quarter-cycle peak
    REQUIRE(m.buffer_l()[2] == Approx(1.f).margin(1e-4f));
}

// ---------------------------------------------------------------------------
// Shared with PeekModule
// ---------------------------------------------------------------------------

TEST_CASE("BufferModule: shared with PeekModule — peek reads buffer_l via shared_ptr", "[buffer]") {
    // N=4; buffer_l set to known values.
    // idx=0.5 → pos_f = 0.5*3 = 1.5 → lerp(buf[1], buf[2], 0.5) = 15
    // idx=1/3 → pos_f = 1.0 exactly → buf[1] = 10
    BufferModule b(4);
    b.buffer_l() = {0.f, 10.f, 20.f, 30.f};

    PeekModule p(PeekInterp::Linear, b.buffer_l_ptr());

    REQUIRE(peek_step(p, 0.5f) == Approx(15.f).margin(1e-4f));
    REQUIRE(peek_step(p, 1.f / 3.f) == Approx(10.f).margin(1e-4f));
    REQUIRE(peek_step(p, 1.f) == Approx(30.f).margin(1e-4f));
}

TEST_CASE("BufferModule: shared with PeekModule — mutation visible through peek", "[buffer]") {
    BufferModule b(4);
    b.buffer_l() = {0.f, 0.f, 0.f, 0.f};
    PeekModule p(PeekInterp::None, b.buffer_l_ptr());

    // Initially zero
    REQUIRE(peek_step(p, 0.f) == 0.f);

    // Write into buffer_l after construction — peek sees the new value immediately.
    b.buffer_l()[0] = 7.f;
    REQUIRE(peek_step(p, 0.f) == Approx(7.f));
}

TEST_CASE("BufferModule: shared with PeekModule — two peeks share same buffer", "[buffer]") {
    BufferModule b(4);
    b.buffer_l() = {1.f, 2.f, 3.f, 4.f};

    PeekModule p1(PeekInterp::None, b.buffer_l_ptr());
    PeekModule p2(PeekInterp::None, b.buffer_l_ptr());

    // Both read slot 0 → same value
    REQUIRE(peek_step(p1, 0.f) == Approx(1.f));
    REQUIRE(peek_step(p2, 0.f) == Approx(1.f));

    // Mutate; both see change
    b.buffer_l()[0] = 99.f;
    REQUIRE(peek_step(p1, 0.f) == Approx(99.f));
    REQUIRE(peek_step(p2, 0.f) == Approx(99.f));
}

// ---------------------------------------------------------------------------
// Shared with PokeModule
// ---------------------------------------------------------------------------

TEST_CASE("BufferModule: shared with PokeModule — poke write visible in buffer", "[buffer]") {
    // N=4; write-pos=1/3 → pos_f=1 → floor → slot 1.
    BufferModule b(4);
    PokeModule   pk(b.buffer_l_ptr(), b.buffer_r_ptr());

    poke_step(pk, 0.7f, 0.3f, 1.f / 3.f, 0.f, 1.f);

    REQUIRE(b.buffer_l()[1] == Approx(0.7f));
    REQUIRE(b.buffer_r()[1] == Approx(0.3f));
}

TEST_CASE("BufferModule: shared with PokeModule — gate=0 does not write", "[buffer]") {
    BufferModule b(4);
    b.buffer_l() = {1.f, 2.f, 3.f, 4.f};
    PokeModule pk(b.buffer_l_ptr(), b.buffer_r_ptr());

    poke_step(pk, 99.f, 99.f, 0.f, 0.f, 0.f); // gate=0

    REQUIRE(b.buffer_l()[0] == Approx(1.f)); // unchanged
}

// ---------------------------------------------------------------------------
// Buffer + Peek + Poke composition (three-way sharing)
// ---------------------------------------------------------------------------

TEST_CASE("BufferModule: buffer+poke+peek three-way composition", "[buffer]") {
    // poke writes; peek reads; all sharing the same L buffer via BufferModule.
    // N=4; write slot 2 (write-pos=2/3), then read it back via peek at idx=2/3.
    BufferModule b(4);
    PokeModule   pk(b.buffer_l_ptr(), b.buffer_r_ptr());
    PeekModule   p(PeekInterp::None, b.buffer_l_ptr());

    // Write 0.5 to slot 2 via poke.
    poke_step(pk, 0.5f, 0.f, 2.f / 3.f, 0.f, 1.f);
    REQUIRE(b.buffer_l()[2] == Approx(0.5f));

    // Peek reads slot 2: idx=2/3 → pos_f=2.0 → NN floor → slot 2.
    REQUIRE(peek_step(p, 2.f / 3.f) == Approx(0.5f));
}

TEST_CASE("BufferModule: buffer_l_ptr and buffer_r_ptr return same shared_ptr", "[buffer]") {
    BufferModule b(4);
    // Verify the ptr returned is the same object as what buffer_l() accesses.
    b.buffer_l_ptr()->at(0) = 42.f;
    REQUIRE(b.buffer_l()[0] == Approx(42.f));

    b.buffer_r_ptr()->at(0) = 7.f;
    REQUIRE(b.buffer_r()[0] == Approx(7.f));
}

TEST_CASE("BufferModule: shared constructor stores provided shared_ptrs", "[buffer]") {
    auto l  = std::make_shared<std::vector<float>>(4, 0.f);
    auto r  = std::make_shared<std::vector<float>>(4, 0.f);
    (*l)[0] = 11.f;
    (*r)[0] = 22.f;

    BufferModule b(l, r);
    REQUIRE(b.buffer_l()[0] == Approx(11.f));
    REQUIRE(b.buffer_r()[0] == Approx(22.f));
    // Same pointer — not a copy.
    REQUIRE(b.buffer_l_ptr() == l);
    REQUIRE(b.buffer_r_ptr() == r);
}
