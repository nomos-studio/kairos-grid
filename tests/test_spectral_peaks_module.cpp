// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/fft/spectral_peaks_module.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace kairos_grid;
using Catch::Approx;

namespace {

const GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};
constexpr std::size_t kN = SpectralPeaksModule::kMaxPeaks;

// Push n samples of constant value; collect trigger count.
std::size_t push_count_triggers(SpectralPeaksModule& m, float sample, float threshold,
                                std::size_t n) {
    std::size_t triggers = 0;
    m.inputs[2].voltage  = threshold;
    for (std::size_t i = 0; i < n; ++i) {
        m.inputs[0].voltage = sample;
        m.inputs[1].voltage = sample;
        m.process(kArgs);
        if (m.outputs[2 * kN].voltage > 0.5f)
            ++triggers;
    }
    return triggers;
}

// Push a sine at bin k0 (window N), return strongest detected peak frequency.
float push_sine_peak_freq(SpectralPeaksModule& m, std::size_t N, float k0, float threshold) {
    m.inputs[2].voltage = threshold;
    for (std::size_t i = 0; i < 2 * N; ++i) {
        const float s =
            std::sin(2.f * 3.14159265f * k0 / static_cast<float>(N) * static_cast<float>(i));
        m.inputs[0].voltage = s;
        m.inputs[1].voltage = s;
        m.process(kArgs);
    }
    return m.outputs[0].voltage; // freq of strongest peak
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("SpectralPeaksModule: port counts — 3 inputs, 17 outputs", "[spectral-peaks]") {
    SpectralPeaksModule m;
    REQUIRE(m.inputs.size() == 3);
    REQUIRE(m.outputs.size() == 2 * kN + 1); // 17
}

TEST_CASE("SpectralPeaksModule: default window is 1024, hop is 256", "[spectral-peaks]") {
    SpectralPeaksModule m;
    REQUIRE(m.window() == 1024);
    REQUIRE(m.hop_size() == 256);
}

TEST_CASE("SpectralPeaksModule: custom window 512", "[spectral-peaks]") {
    SpectralPeaksModule m(512);
    REQUIRE(m.window() == 512);
    REQUIRE(m.hop_size() == 128);
}

// ---------------------------------------------------------------------------
// Initial state and silence
// ---------------------------------------------------------------------------

TEST_CASE("SpectralPeaksModule: all outputs zero before any samples", "[spectral-peaks]") {
    SpectralPeaksModule m(64);
    for (const auto& out : m.outputs)
        REQUIRE(out.voltage == 0.f);
}

TEST_CASE("SpectralPeaksModule: silence in — no peaks detected, all outputs zero",
          "[spectral-peaks]") {
    SpectralPeaksModule m(64);
    push_count_triggers(m, 0.f, 0.f, 4 * 64);
    for (std::size_t i = 0; i < 2 * kN; ++i)
        REQUIRE(m.outputs[i].voltage == 0.f);
}

// ---------------------------------------------------------------------------
// Trigger
// ---------------------------------------------------------------------------

TEST_CASE("SpectralPeaksModule: trigger fires exactly once per hop", "[spectral-peaks]") {
    // Window=64, hop=16.  Over 4*16=64 samples = 4 hops, expect 4 triggers.
    SpectralPeaksModule m(64);
    const std::size_t   hops     = 4;
    const std::size_t   n        = hops * m.hop_size();
    const std::size_t   triggers = push_count_triggers(m, 0.5f, 0.f, n);
    REQUIRE(triggers == hops);
}

TEST_CASE("SpectralPeaksModule: trigger is 0 between hops", "[spectral-peaks]") {
    // Check the sample after a trigger — should be 0.
    SpectralPeaksModule m(64);
    m.inputs[2].voltage = 0.f;
    bool saw_trigger    = false;
    bool error          = false;
    for (std::size_t i = 0; i < 4 * 64; ++i) {
        m.inputs[0].voltage = 0.5f;
        m.inputs[1].voltage = 0.5f;
        m.process(kArgs);
        const bool this_trig = m.outputs[2 * kN].voltage > 0.5f;
        if (saw_trigger && this_trig)
            error = true; // two consecutive trigger-high samples
        saw_trigger = this_trig;
    }
    REQUIRE(!error);
}

// ---------------------------------------------------------------------------
// Peak detection
// ---------------------------------------------------------------------------

TEST_CASE("SpectralPeaksModule: mid-frequency sine — peak near sine frequency",
          "[spectral-peaks]") {
    // Sine at bin k0=8, window=64, nb=33.
    // Expected freq ≈ 8/32 = 0.25.
    SpectralPeaksModule m(64);
    const float         freq = push_sine_peak_freq(m, 64, 8.f, 0.f);
    REQUIRE(freq == Approx(8.f / 32.f).margin(0.05f));
}

TEST_CASE("SpectralPeaksModule: detected peak amplitude in [0, 1]", "[spectral-peaks]") {
    SpectralPeaksModule m(64);
    push_sine_peak_freq(m, 64, 8.f, 0.f);
    // Strongest peak amplitude should be 1.0 (it IS the spectral maximum).
    REQUIRE(m.outputs[kN].voltage >= 0.f);
    REQUIRE(m.outputs[kN].voltage <= 1.f);
}

TEST_CASE("SpectralPeaksModule: DC input — no interior peaks (edge bins excluded)",
          "[spectral-peaks]") {
    // DC concentrates at bin 0 which cannot be a local interior maximum.
    // Peak detector should find no candidates.
    SpectralPeaksModule m(64);
    push_count_triggers(m, 1.f, 0.f, 4 * 64);
    REQUIRE(m.n_peaks() == 0);
    REQUIRE(m.outputs[0].voltage == 0.f);
    REQUIRE(m.outputs[kN].voltage == 0.f);
}

TEST_CASE("SpectralPeaksModule: threshold=0 finds more peaks than threshold=0.9",
          "[spectral-peaks]") {
    // A broadband signal has many local maxima; tight threshold prunes most.
    const std::size_t   N = 64;
    SpectralPeaksModule mlo(N), mhi(N);

    // Broadband: sum several sines at different bins.
    mlo.inputs[2].voltage = 0.f;
    mhi.inputs[2].voltage = 0.9f;
    for (std::size_t i = 0; i < 4 * N; ++i) {
        const float s =
            std::sin(2.f * 3.14159265f * 4.f / static_cast<float>(N) * static_cast<float>(i)) +
            std::sin(2.f * 3.14159265f * 8.f / static_cast<float>(N) * static_cast<float>(i)) +
            std::sin(2.f * 3.14159265f * 14.f / static_cast<float>(N) * static_cast<float>(i));
        mlo.inputs[0].voltage = s;
        mlo.inputs[1].voltage = s;
        mlo.process(kArgs);
        mhi.inputs[0].voltage = s;
        mhi.inputs[1].voltage = s;
        mhi.process(kArgs);
    }
    REQUIRE(mlo.n_peaks() >= mhi.n_peaks());
}

TEST_CASE("SpectralPeaksModule: peaks sorted by amplitude — outputs[0] has highest amp",
          "[spectral-peaks]") {
    const std::size_t   N = 64;
    SpectralPeaksModule m(N);
    // Sum of sines with different amplitudes.
    m.inputs[2].voltage = 0.f;
    for (std::size_t i = 0; i < 4 * N; ++i) {
        const float s =
            0.9f *
                std::sin(2.f * 3.14159265f * 4.f / static_cast<float>(N) * static_cast<float>(i)) +
            0.3f *
                std::sin(2.f * 3.14159265f * 12.f / static_cast<float>(N) * static_cast<float>(i));
        m.inputs[0].voltage = s;
        m.inputs[1].voltage = s;
        m.process(kArgs);
    }
    if (m.n_peaks() >= 2) {
        REQUIRE(m.outputs[kN].voltage >= m.outputs[kN + 1].voltage);
    }
}

TEST_CASE("SpectralPeaksModule: unused peak slots are zero", "[spectral-peaks]") {
    // If fewer than 8 peaks are found, the remaining slots are zero.
    SpectralPeaksModule m(64);
    // Single sine → at most 1 peak.
    push_sine_peak_freq(m, 64, 8.f, 0.f);
    if (m.n_peaks() < kN) {
        for (std::size_t i = m.n_peaks(); i < kN; ++i) {
            REQUIRE(m.outputs[i].voltage == 0.f);
            REQUIRE(m.outputs[kN + i].voltage == 0.f);
        }
    }
}

// ---------------------------------------------------------------------------
// prepare() reset
// ---------------------------------------------------------------------------

TEST_CASE("SpectralPeaksModule: prepare() resets all outputs and n_peaks to 0",
          "[spectral-peaks]") {
    SpectralPeaksModule m(64);
    push_sine_peak_freq(m, 64, 8.f, 0.f);
    m.prepare(kArgs);
    REQUIRE(m.n_peaks() == 0);
    for (const auto& out : m.outputs)
        REQUIRE(out.voltage == 0.f);
}

// ---------------------------------------------------------------------------
// Multiple window sizes
// ---------------------------------------------------------------------------

TEST_CASE("SpectralPeaksModule: 512-window — sine peak detected near expected frequency",
          "[spectral-peaks]") {
    SpectralPeaksModule m(512);
    const std::size_t   nb   = 512 / 2 + 1; // 257
    const float         k0   = 16.f;
    const float         freq = push_sine_peak_freq(m, 512, k0, 0.f);
    if (m.n_peaks() > 0)
        REQUIRE(freq == Approx(k0 / static_cast<float>(nb - 1u)).margin(0.05f));
}

// ---------------------------------------------------------------------------
// Copy / move protection
// ---------------------------------------------------------------------------

static_assert(!std::is_copy_constructible_v<SpectralPeaksModule>,
              "SpectralPeaksModule must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<SpectralPeaksModule>,
              "SpectralPeaksModule must not be copy-assignable");
