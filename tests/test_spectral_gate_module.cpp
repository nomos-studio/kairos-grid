// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/fft/spectral_gate_module.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numeric>

using namespace kairos_grid;
using Catch::Approx;

namespace {

const GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};

void push(SpectralGateModule& m, float sample_l, float sample_r, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        m.inputs[0].voltage = sample_l;
        m.inputs[1].voltage = sample_r;
        m.process(kArgs);
    }
}

void push(SpectralGateModule& m, float sample, std::size_t n) {
    push(m, sample, sample, n);
}

// Push n samples and accumulate squared output energy from channel c.
float energy_while_pushing(SpectralGateModule& m, float sample, std::size_t n, int c = 0) {
    float sum = 0.f;
    for (std::size_t i = 0; i < n; ++i) {
        m.inputs[0].voltage = sample;
        m.inputs[1].voltage = sample;
        m.process(kArgs);
        sum += m.outputs[c].voltage * m.outputs[c].voltage;
    }
    return sum;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction and accessors
// ---------------------------------------------------------------------------

TEST_CASE("SpectralGateModule: port counts — 4 inputs, 2 outputs", "[spectral-gate]") {
    SpectralGateModule m;
    REQUIRE(m.inputs.size() == 4);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("SpectralGateModule: default window is 1024, hop is 256", "[spectral-gate]") {
    SpectralGateModule m;
    REQUIRE(m.window() == 1024);
    REQUIRE(m.hop_size() == 256);
}

TEST_CASE("SpectralGateModule: custom window 512 — correct sizes", "[spectral-gate]") {
    SpectralGateModule m(512);
    REQUIRE(m.window() == 512);
    REQUIRE(m.hop_size() == 128);
}

// ---------------------------------------------------------------------------
// Initial state and silence
// ---------------------------------------------------------------------------

TEST_CASE("SpectralGateModule: outputs are 0 before any samples", "[spectral-gate]") {
    SpectralGateModule m(64);
    REQUIRE(m.outputs[0].voltage == 0.f);
    REQUIRE(m.outputs[1].voltage == 0.f);
}

TEST_CASE("SpectralGateModule: silence in → silence out", "[spectral-gate]") {
    SpectralGateModule m(64);
    m.inputs[2].voltage = 0.5f; // threshold
    m.inputs[3].voltage = 0.f;  // floor
    const float e       = energy_while_pushing(m, 0.f, 2 * 64);
    REQUIRE(e == 0.f);
}

// ---------------------------------------------------------------------------
// Pass-through behaviour (no gating)
// ---------------------------------------------------------------------------

TEST_CASE("SpectralGateModule: threshold=0 passes all bins — output non-zero after warm-up",
          "[spectral-gate]") {
    // gate_threshold = max_mag * 0 = 0; all mag[k] >= 0, so all bins pass at scale=1.
    SpectralGateModule m(64);
    m.inputs[2].voltage = 0.f; // threshold = 0 → gate nothing
    m.inputs[3].voltage = 0.f; // floor = 0 (irrelevant; nothing is gated)
    push(m, 1.f, 2 * 64);      // warm up
    const float e = energy_while_pushing(m, 1.f, 64);
    REQUIRE(e > 0.f);
}

TEST_CASE("SpectralGateModule: floor=1 acts as pass-through regardless of threshold",
          "[spectral-gate]") {
    // When floor=1, gated bins get scale=1 — same as passed bins — so no attenuation.
    SpectralGateModule m(64);
    m.inputs[2].voltage = 1.f; // threshold = 1 (only single peak bin normally passes)
    m.inputs[3].voltage = 1.f; // floor = 1 → gated bins also pass at full scale
    push(m, 1.f, 2 * 64);
    const float e = energy_while_pushing(m, 1.f, 64);
    REQUIRE(e > 0.f);
}

// ---------------------------------------------------------------------------
// Gate attenuation
// ---------------------------------------------------------------------------

TEST_CASE("SpectralGateModule: floor=0, high threshold — output energy less than floor=1",
          "[spectral-gate]") {
    // With the same threshold, floor=0 gates bins to zero; floor=1 lets them through.
    // Energy must be less (or equal) with floor=0 than floor=1.
    const std::size_t warm = 128, measure = 64;
    auto              run = [&](float floor_val) {
        SpectralGateModule m(64);
        m.inputs[2].voltage = 0.7f; // moderate threshold
        m.inputs[3].voltage = floor_val;
        push(m, 0.5f, warm);
        return energy_while_pushing(m, 0.5f, measure);
    };
    REQUIRE(run(0.f) <= run(1.f));
}

TEST_CASE("SpectralGateModule: floor=0.5 gives more energy than floor=0 with same threshold",
          "[spectral-gate]") {
    // A mid-frequency sine has a sharp spectral peak; most bins are below threshold.
    // floor=0.5 partially passes the gated bins; floor=0 silences them.
    const std::size_t N    = 64;
    const float       k0   = 8.f;
    const std::size_t warm = 2 * N;
    const std::size_t meas = N;

    auto run = [&](float floor_val) {
        SpectralGateModule m(N);
        m.inputs[2].voltage = 0.5f;
        m.inputs[3].voltage = floor_val;
        for (std::size_t i = 0; i < warm + meas; ++i) {
            const float s =
                std::sin(2.f * 3.14159265f * k0 / static_cast<float>(N) * static_cast<float>(i));
            m.inputs[0].voltage = s;
            m.inputs[1].voltage = s;
            m.process(kArgs);
        }
        float e = 0.f;
        // Measure one more window of output from the already-processed state.
        for (std::size_t i = 0; i < meas; ++i) {
            m.inputs[0].voltage = 0.f;
            m.inputs[1].voltage = 0.f;
            m.process(kArgs);
            e += m.outputs[0].voltage * m.outputs[0].voltage;
        }
        return e;
    };
    REQUIRE(run(0.5f) >= run(0.f));
}

// ---------------------------------------------------------------------------
// Spectral selectivity
// ---------------------------------------------------------------------------

TEST_CASE("SpectralGateModule: DC signal — dominant bin preserved at high threshold",
          "[spectral-gate]") {
    // DC concentrates energy at bin 0 (the spectral peak).  With floor=0 and
    // threshold just below 1, only bin 0 passes, but it does pass — output non-zero.
    SpectralGateModule m(64);
    m.inputs[2].voltage = 0.95f; // threshold near 1 — only peak bin passes
    m.inputs[3].voltage = 0.f;   // floor = 0 — hard gate
    push(m, 1.f, 2 * 64);        // warm with DC
    const float e = energy_while_pushing(m, 1.f, 64);
    REQUIRE(e > 0.f); // bin 0 is the peak; it passes
}

TEST_CASE("SpectralGateModule: threshold=1, floor=0 — only max-magnitude bin passes",
          "[spectral-gate]") {
    // gate_threshold = max_mag * 1 = max_mag.
    // Condition: mag[k] >= max_mag — exactly one bin satisfies this (the peak).
    // Output energy drastically reduced but non-zero (peak bin survives).
    SpectralGateModule m(64);
    m.inputs[2].voltage = 1.f; // threshold = 1
    m.inputs[3].voltage = 0.f; // floor = 0

    // Measure with threshold=0 (all pass) for comparison.
    const float e_full = [&]() {
        SpectralGateModule ref(64);
        ref.inputs[2].voltage = 0.f;
        ref.inputs[3].voltage = 0.f;
        push(ref, 1.f, 2 * 64);
        return energy_while_pushing(ref, 1.f, 64);
    }();

    push(m, 1.f, 2 * 64);
    const float e_gate = energy_while_pushing(m, 1.f, 64);

    REQUIRE(e_gate >= 0.f);            // never negative energy
    REQUIRE(e_gate <= e_full + 1e-4f); // peak-only ≤ all-pass
}

// ---------------------------------------------------------------------------
// Stability
// ---------------------------------------------------------------------------

TEST_CASE("SpectralGateModule: output bounded after gating — no blow-up", "[spectral-gate]") {
    SpectralGateModule m(64);
    // Alternate between threshold extremes every hop to stress the gate.
    for (int rep = 0; rep < 16; ++rep) {
        m.inputs[2].voltage = (rep % 2 == 0) ? 0.f : 1.f;
        m.inputs[3].voltage = (rep % 3 == 0) ? 0.f : 1.f;
        push(m, 0.8f, 16); // one hop worth of samples
        REQUIRE(m.outputs[0].voltage >= -2.f);
        REQUIRE(m.outputs[0].voltage <= 2.f);
    }
}

// ---------------------------------------------------------------------------
// prepare() reset
// ---------------------------------------------------------------------------

TEST_CASE("SpectralGateModule: prepare() resets output and accumulator to zero",
          "[spectral-gate]") {
    SpectralGateModule m(64);
    m.inputs[2].voltage = 0.f;
    m.inputs[3].voltage = 0.f;
    push(m, 1.f, 64);
    m.prepare(kArgs);

    // Output ports zeroed immediately.
    REQUIRE(m.outputs[0].voltage == 0.f);
    REQUIRE(m.outputs[1].voltage == 0.f);

    // Accumulator is cleared — next samples produce no residual from old state.
    const float e = energy_while_pushing(m, 0.f, 64);
    REQUIRE(e == 0.f);
}

// ---------------------------------------------------------------------------
// Stereo independence
// ---------------------------------------------------------------------------

TEST_CASE("SpectralGateModule: stereo channels are independent", "[spectral-gate]") {
    // L gets full-amplitude DC; R gets silence.  With threshold=0, L should have
    // energy; R should be silent.
    SpectralGateModule m(64);
    m.inputs[2].voltage = 0.f; // threshold = 0
    m.inputs[3].voltage = 0.f;
    push(m, 1.f, 0.f, 2 * 64);                            // L=DC, R=silence, warm up
    const float el = energy_while_pushing(m, 1.f, 64, 0); // L channel
    // Measure R separately.
    SpectralGateModule mr(64);
    mr.inputs[2].voltage = 0.f;
    mr.inputs[3].voltage = 0.f;
    push(mr, 1.f, 0.f, 2 * 64);
    float er = 0.f;
    for (std::size_t i = 0; i < 64; ++i) {
        mr.inputs[0].voltage = 1.f;
        mr.inputs[1].voltage = 0.f;
        mr.process(kArgs);
        er += mr.outputs[1].voltage * mr.outputs[1].voltage;
    }
    REQUIRE(el > 0.f);                        // L has energy (DC input)
    REQUIRE(er == Approx(0.f).margin(1e-6f)); // R stays silent
}

// ---------------------------------------------------------------------------
// Multiple window sizes
// ---------------------------------------------------------------------------

TEST_CASE("SpectralGateModule: 512-window — threshold=0 passes all bins", "[spectral-gate]") {
    SpectralGateModule m(512);
    m.inputs[2].voltage = 0.f;
    m.inputs[3].voltage = 0.f;
    push(m, 1.f, 2 * 512);
    const float e = energy_while_pushing(m, 1.f, 128);
    REQUIRE(e > 0.f);
}

TEST_CASE("SpectralGateModule: 2048-window — floor=1 gives output", "[spectral-gate]") {
    SpectralGateModule m(2048);
    m.inputs[2].voltage = 1.f; // extreme threshold
    m.inputs[3].voltage = 1.f; // floor=1 → all pass
    push(m, 1.f, 2 * 2048);
    const float e = energy_while_pushing(m, 1.f, 512);
    REQUIRE(e > 0.f);
}

// ---------------------------------------------------------------------------
// Copy / move protection
// ---------------------------------------------------------------------------

static_assert(!std::is_copy_constructible_v<SpectralGateModule>,
              "SpectralGateModule must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<SpectralGateModule>,
              "SpectralGateModule must not be copy-assignable");
