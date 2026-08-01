// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/fft/bin_shift_module.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace kairos_grid;
using Catch::Approx;

namespace {

const GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};

void push(BinShiftModule& m, float sample_l, float sample_r, float shift, std::size_t n) {
    m.inputs[2].voltage = shift;
    for (std::size_t i = 0; i < n; ++i) {
        m.inputs[0].voltage = sample_l;
        m.inputs[1].voltage = sample_r;
        m.process(kArgs);
    }
}

void push(BinShiftModule& m, float sample, float shift, std::size_t n) {
    push(m, sample, sample, shift, n);
}

// Push n samples, accumulate squared output energy from channel c.
float energy_while_pushing(BinShiftModule& m, float sample, float shift, std::size_t n, int c = 0) {
    m.inputs[2].voltage = shift;
    float sum           = 0.f;
    for (std::size_t i = 0; i < n; ++i) {
        m.inputs[0].voltage = sample;
        m.inputs[1].voltage = sample;
        m.process(kArgs);
        sum += m.outputs[c].voltage * m.outputs[c].voltage;
    }
    return sum;
}

// Push a sine at bin k0 (out of window N), accumulate energy from channel c.
float sine_energy(BinShiftModule& m, std::size_t N, float k0, float shift, std::size_t n,
                  int c = 0) {
    m.inputs[2].voltage = shift;
    float sum           = 0.f;
    for (std::size_t i = 0; i < n; ++i) {
        const float s =
            std::sin(2.f * 3.14159265f * k0 / static_cast<float>(N) * static_cast<float>(i));
        m.inputs[0].voltage = s;
        m.inputs[1].voltage = s;
        m.process(kArgs);
        sum += m.outputs[c].voltage * m.outputs[c].voltage;
    }
    return sum;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction and accessors
// ---------------------------------------------------------------------------

TEST_CASE("BinShiftModule: port counts — 3 inputs, 2 outputs", "[bin-shift]") {
    BinShiftModule m;
    REQUIRE(m.inputs.size() == 3);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("BinShiftModule: default window is 1024, hop is 256", "[bin-shift]") {
    BinShiftModule m;
    REQUIRE(m.window() == 1024);
    REQUIRE(m.hop_size() == 256);
}

TEST_CASE("BinShiftModule: custom window 512", "[bin-shift]") {
    BinShiftModule m(512);
    REQUIRE(m.window() == 512);
    REQUIRE(m.hop_size() == 128);
}

// ---------------------------------------------------------------------------
// Initial state and silence
// ---------------------------------------------------------------------------

TEST_CASE("BinShiftModule: outputs are 0 before any samples", "[bin-shift]") {
    BinShiftModule m(64);
    REQUIRE(m.outputs[0].voltage == 0.f);
    REQUIRE(m.outputs[1].voltage == 0.f);
}

TEST_CASE("BinShiftModule: silence in → silence out for any shift", "[bin-shift]") {
    for (float shift : {-1.f, -0.5f, 0.f, 0.5f, 1.f}) {
        BinShiftModule m(64);
        const float    e = energy_while_pushing(m, 0.f, shift, 2 * 64);
        REQUIRE(e == 0.f);
    }
}

// ---------------------------------------------------------------------------
// Round-trip (shift = 0)
// ---------------------------------------------------------------------------

TEST_CASE("BinShiftModule: shift=0 passes all bins — output non-zero after warm-up",
          "[bin-shift]") {
    // shift=0 → s=0 → synth_cpx[k] = fwd_out[k] → pure STFT round-trip.
    BinShiftModule m(64);
    push(m, 1.f, 0.f, 2 * 64); // warm up
    const float e = energy_while_pushing(m, 1.f, 0.f, 64);
    REQUIRE(e > 0.f);
}

TEST_CASE("BinShiftModule: shift=0 gives more energy than large upshift", "[bin-shift]") {
    // With shift=0 all content passes; with shift=0.9 most content is pushed
    // past Nyquist and zeroed.  Round-trip energy > large-shift energy.
    const std::size_t warm = 2 * 64, meas = 64;
    BinShiftModule    mz(64), ms(64);

    push(mz, 1.f, 0.f, warm);
    push(ms, 1.f, 0.9f, warm);

    const float ez = energy_while_pushing(mz, 1.f, 0.f, meas);
    const float es = energy_while_pushing(ms, 1.f, 0.9f, meas);
    REQUIRE(ez > es);
}

// ---------------------------------------------------------------------------
// Upshift and downshift
// ---------------------------------------------------------------------------

TEST_CASE("BinShiftModule: moderate upshift — output non-zero (content stays in band)",
          "[bin-shift]") {
    // shift=0.2 shifts content upward; some bins still fall inside [0, nb).
    BinShiftModule m(64);
    push(m, 1.f, 0.2f, 2 * 64);
    const float e = energy_while_pushing(m, 1.f, 0.2f, 64);
    REQUIRE(e > 0.f);
}

TEST_CASE("BinShiftModule: moderate downshift — output non-zero (content stays in band)",
          "[bin-shift]") {
    // shift=-0.2 shifts content downward; some bins still fall inside [0, nb).
    BinShiftModule m(64);
    push(m, 1.f, -0.2f, 2 * 64);
    const float e = energy_while_pushing(m, 1.f, -0.2f, 64);
    REQUIRE(e > 0.f);
}

TEST_CASE("BinShiftModule: sine shifted past Nyquist → output much quieter than unshifted",
          "[bin-shift]") {
    // Sine at bin k0=8, window=64, nb=33.
    // shift=0: s=0 → bin 8 passes → energy present.
    // shift=0.85: s=round(0.85*32)=round(27.2)=27 → sine needs output bin 8+27=35>32 → zeroed.
    const std::size_t N  = 64;
    const float       k0 = 8.f;
    const std::size_t n  = 4 * N; // 4 windows to warm up and measure

    BinShiftModule mref(N), mshifted(N);
    const float    e_ref     = sine_energy(mref, N, k0, 0.f, n);
    const float    e_shifted = sine_energy(mshifted, N, k0, 0.85f, n);

    REQUIRE(e_ref > 0.f);
    REQUIRE(e_shifted < e_ref);
}

TEST_CASE("BinShiftModule: downshift moves content below DC → output much quieter", "[bin-shift]") {
    // Sine at bin k0=8, window=64, nb=33.
    // shift=-0.85: s=-27 → synth_cpx[k] = fwd_out[k+27]; for k=8, src=35>32 → zero.
    const std::size_t N  = 64;
    const float       k0 = 8.f;
    const std::size_t n  = 4 * N;

    BinShiftModule mref(N), mshifted(N);
    const float    e_ref     = sine_energy(mref, N, k0, 0.f, n);
    const float    e_shifted = sine_energy(mshifted, N, k0, -0.85f, n);

    REQUIRE(e_ref > 0.f);
    REQUIRE(e_shifted < e_ref);
}

// ---------------------------------------------------------------------------
// Shift at extremes
// ---------------------------------------------------------------------------

TEST_CASE("BinShiftModule: shift=+1 (maximum) — output near-zero for DC input", "[bin-shift]") {
    // s = nb-1: synth_cpx[k] = fwd_out[k-(nb-1)]; only k=nb-1 gets fwd_out[0] (DC).
    // All other bins zeroed.  DC content lands at Nyquist — output is very small.
    BinShiftModule m(64);
    push(m, 1.f, 1.f, 4 * 64);
    const float e_max_shift = energy_while_pushing(m, 1.f, 1.f, 64);

    BinShiftModule mref(64);
    push(mref, 1.f, 0.f, 4 * 64);
    const float e_no_shift = energy_while_pushing(mref, 1.f, 0.f, 64);

    REQUIRE(e_max_shift < e_no_shift);
}

TEST_CASE("BinShiftModule: shift=-1 (maximum downshift) — output near-zero for DC input",
          "[bin-shift]") {
    // s = -(nb-1): synth_cpx[k] = fwd_out[k+(nb-1)]; only k=0 gets fwd_out[nb-1] (Nyquist).
    // Most bins zeroed.
    BinShiftModule m(64);
    push(m, 1.f, -1.f, 4 * 64);
    const float e_max_dn = energy_while_pushing(m, 1.f, -1.f, 64);

    BinShiftModule mref(64);
    push(mref, 1.f, 0.f, 4 * 64);
    const float e_no_shift = energy_while_pushing(mref, 1.f, 0.f, 64);

    REQUIRE(e_max_dn < e_no_shift);
}

// ---------------------------------------------------------------------------
// Stability
// ---------------------------------------------------------------------------

TEST_CASE("BinShiftModule: output bounded for sweeping shift CV", "[bin-shift]") {
    BinShiftModule m(64);
    // Sweep shift from -1 to +1 while processing one hop at a time.
    for (int step = 0; step <= 16; ++step) {
        const float shift = -1.f + 2.f * static_cast<float>(step) / 16.f;
        push(m, 0.7f, shift, 16);
        REQUIRE(m.outputs[0].voltage >= -2.f);
        REQUIRE(m.outputs[0].voltage <= 2.f);
    }
}

// ---------------------------------------------------------------------------
// prepare() reset
// ---------------------------------------------------------------------------

TEST_CASE("BinShiftModule: prepare() resets output and accumulator", "[bin-shift]") {
    BinShiftModule m(64);
    push(m, 1.f, 0.f, 64);
    m.prepare(kArgs);

    REQUIRE(m.outputs[0].voltage == 0.f);
    REQUIRE(m.outputs[1].voltage == 0.f);

    const float e = energy_while_pushing(m, 0.f, 0.f, 64);
    REQUIRE(e == 0.f);
}

// ---------------------------------------------------------------------------
// Stereo independence
// ---------------------------------------------------------------------------

TEST_CASE("BinShiftModule: stereo channels are independent", "[bin-shift]") {
    // L gets DC; R gets silence.  With shift=0, L should have energy; R should not.
    BinShiftModule m(64);
    m.inputs[2].voltage = 0.f;
    push(m, 1.f, 0.f, 0.f, 2 * 64);

    float el = 0.f, er = 0.f;
    for (std::size_t i = 0; i < 64; ++i) {
        m.inputs[0].voltage = 1.f;
        m.inputs[1].voltage = 0.f;
        m.process(kArgs);
        el += m.outputs[0].voltage * m.outputs[0].voltage;
        er += m.outputs[1].voltage * m.outputs[1].voltage;
    }
    REQUIRE(el > 0.f);
    REQUIRE(er == Approx(0.f).margin(1e-6f));
}

// ---------------------------------------------------------------------------
// Multiple window sizes
// ---------------------------------------------------------------------------

TEST_CASE("BinShiftModule: 512-window — shift=0 produces non-zero output", "[bin-shift]") {
    BinShiftModule m(512);
    push(m, 1.f, 0.f, 2 * 512);
    const float e = energy_while_pushing(m, 1.f, 0.f, 128);
    REQUIRE(e > 0.f);
}

TEST_CASE("BinShiftModule: 2048-window — moderate shift produces non-zero output", "[bin-shift]") {
    BinShiftModule m(2048);
    push(m, 1.f, 0.1f, 2 * 2048);
    const float e = energy_while_pushing(m, 1.f, 0.1f, 512);
    REQUIRE(e > 0.f);
}

// ---------------------------------------------------------------------------
// Copy / move protection
// ---------------------------------------------------------------------------

static_assert(!std::is_copy_constructible_v<BinShiftModule>,
              "BinShiftModule must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<BinShiftModule>,
              "BinShiftModule must not be copy-assignable");
