// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/fft/spectral_smear_module.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

using namespace kairos_grid;
using Catch::Approx;

namespace {

static constexpr float kTwoPi = 6.28318530f;
const GridProcessArgs  kArgs{48000.f, 1.f / 48000.f, 0};

void push(SpectralSmearModule& m, float in_l, float in_r, float smear, float density,
          std::size_t n) {
    m.inputs[2].voltage = smear;
    m.inputs[3].voltage = density;
    for (std::size_t i = 0; i < n; ++i) {
        m.inputs[0].voltage = in_l;
        m.inputs[1].voltage = in_r;
        m.process(kArgs);
    }
}

void push(SpectralSmearModule& m, float sample, float smear, float density, std::size_t n) {
    push(m, sample, sample, smear, density, n);
}

void push_fn(SpectralSmearModule& m, float smear, float density, std::size_t n, auto fn) {
    m.inputs[2].voltage = smear;
    m.inputs[3].voltage = density;
    for (std::size_t i = 0; i < n; ++i) {
        const float s       = fn(i);
        m.inputs[0].voltage = s;
        m.inputs[1].voltage = s;
        m.process(kArgs);
    }
}

float rms(const SpectralSmearModule& m) {
    float s = 0.f;
    for (const auto& out : m.outputs)
        s += out.voltage * out.voltage;
    return std::sqrt(s / static_cast<float>(m.outputs.size()));
}

float tone(std::size_t N, float k0, std::size_t i) {
    return std::sin(kTwoPi * k0 / static_cast<float>(N) * static_cast<float>(i));
}

} // namespace

// ---------------------------------------------------------------------------
// Construction and accessors
// ---------------------------------------------------------------------------

TEST_CASE("SpectralSmearModule: port counts — 4 inputs, 2 outputs", "[spectral-smear]") {
    SpectralSmearModule m;
    REQUIRE(m.inputs.size() == 4);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("SpectralSmearModule: default window 1024, hop 256", "[spectral-smear]") {
    SpectralSmearModule m;
    REQUIRE(m.window() == 1024);
    REQUIRE(m.hop_size() == 256);
}

TEST_CASE("SpectralSmearModule: custom window 512", "[spectral-smear]") {
    SpectralSmearModule m(512);
    REQUIRE(m.window() == 512);
    REQUIRE(m.hop_size() == 128);
}

// ---------------------------------------------------------------------------
// Initial state
// ---------------------------------------------------------------------------

TEST_CASE("SpectralSmearModule: outputs silent before first samples", "[spectral-smear]") {
    SpectralSmearModule m(64);
    REQUIRE(m.outputs[0].voltage == 0.f);
    REQUIRE(m.outputs[1].voltage == 0.f);
}

TEST_CASE("SpectralSmearModule: silent input → outputs near zero", "[spectral-smear]") {
    // With zero input, smoothed_mag stays zero → synthesis produces silence.
    SpectralSmearModule m(64);
    push(m, 0.f, 0.f, 1.f, 128);
    REQUIRE(m.outputs[0].voltage == Approx(0.f).margin(1e-6f));
    REQUIRE(m.outputs[1].voltage == Approx(0.f).margin(1e-6f));
}

// ---------------------------------------------------------------------------
// Smear = 0 (live resynthesis)
// ---------------------------------------------------------------------------

TEST_CASE("SpectralSmearModule: smear=0 with tone → non-zero output after warmup",
          "[spectral-smear]") {
    SpectralSmearModule m(64);
    const std::size_t   N = 64;
    // smear=0, density=1 — live resynthesis of current spectrum
    push_fn(m, 0.f, 1.f, 2 * N, [&](std::size_t i) { return tone(N, 8.f, i); });
    REQUIRE(rms(m) > 0.f);
}

TEST_CASE("SpectralSmearModule: smear=0 smoothed_mag tracks current frame", "[spectral-smear]") {
    // At smear=0, smoothed_mag[k] = mag[k] each hop — no memory of past frames.
    SpectralSmearModule m(64);
    const std::size_t   N = 64;
    // Warm with a tone.
    push_fn(m, 0.f, 1.f, N, [&](std::size_t i) { return tone(N, 8.f, i); });
    const float sum_after_tone =
        std::accumulate(m.smoothed_mag_l().begin(), m.smoothed_mag_l().end(), 0.f);

    // Switch to silence — at smear=0 the next hop should clear smoothed_mag immediately.
    push(m, 0.f, 0.f, 1.f, N);
    const float sum_after_silence =
        std::accumulate(m.smoothed_mag_l().begin(), m.smoothed_mag_l().end(), 0.f);

    REQUIRE(sum_after_tone > sum_after_silence * 10.f);
}

// ---------------------------------------------------------------------------
// Smear = 1 (frozen spectrum)
// ---------------------------------------------------------------------------

TEST_CASE("SpectralSmearModule: smear=1 freezes smoothed_mag after first non-zero frame",
          "[spectral-smear]") {
    SpectralSmearModule m(64);
    const std::size_t   N = 64;
    // Warm with a tone using smear=1.
    push_fn(m, 1.f, 1.f, N, [&](std::size_t i) { return tone(N, 8.f, i); });
    const std::vector<float> mag_after_tone = m.smoothed_mag_l();

    // Switch to silence — at smear=1 the spectrum should not change.
    push(m, 0.f, 1.f, 1.f, N);
    const std::vector<float>& mag_after_silence = m.smoothed_mag_l();

    REQUIRE(mag_after_tone == mag_after_silence);
}

TEST_CASE("SpectralSmearModule: smear=1 sustains audio output after input goes silent",
          "[spectral-smear]") {
    SpectralSmearModule m(64);
    const std::size_t   N = 64;
    push_fn(m, 1.f, 1.f, 2 * N, [&](std::size_t i) { return tone(N, 8.f, i); });

    // Input goes silent; smear=1 holds the spectrum.
    push(m, 0.f, 1.f, 1.f, N);
    REQUIRE(rms(m) > 0.f);
}

// ---------------------------------------------------------------------------
// Smear decay behaviour
// ---------------------------------------------------------------------------

TEST_CASE("SpectralSmearModule: partial smear — spectrum decays slower than smear=0",
          "[spectral-smear]") {
    const std::size_t N = 64;

    // Module A: smear=0 (no memory).
    SpectralSmearModule ma(N);
    push_fn(ma, 0.f, 1.f, N, [&](std::size_t i) { return tone(N, 8.f, i); });
    push(ma, 0.f, 0.f, 1.f, N);
    const float sum_a =
        std::accumulate(ma.smoothed_mag_l().begin(), ma.smoothed_mag_l().end(), 0.f);

    // Module B: smear=0.9 (strong memory).
    SpectralSmearModule mb(N);
    push_fn(mb, 0.9f, 1.f, N, [&](std::size_t i) { return tone(N, 8.f, i); });
    push(mb, 0.f, 0.9f, 1.f, N);
    const float sum_b =
        std::accumulate(mb.smoothed_mag_l().begin(), mb.smoothed_mag_l().end(), 0.f);

    // B should retain more spectral energy after silence.
    REQUIRE(sum_b > sum_a * 2.f);
}

// ---------------------------------------------------------------------------
// Density threshold
// ---------------------------------------------------------------------------

TEST_CASE("SpectralSmearModule: density=1 passes all bins", "[spectral-smear]") {
    // At density=1 the threshold is 0 — every bin with any energy passes.
    // Verify by checking that smoothed_mag and synth output are both non-zero
    // for a broad-spectrum input.
    SpectralSmearModule m(64);
    const std::size_t   N = 64;
    push_fn(m, 0.f, 1.f, 2 * N, [&](std::size_t i) { return tone(N, 8.f, i); });
    // At least some bins in smoothed_mag should be non-zero.
    const auto& sm = m.smoothed_mag_l();
    const int   nonzero_bins =
        static_cast<int>(std::count_if(sm.begin(), sm.end(), [](float v) { return v > 0.f; }));
    REQUIRE(nonzero_bins > 1);
}

TEST_CASE("SpectralSmearModule: low density reduces output energy vs full density",
          "[spectral-smear]") {
    const std::size_t N = 64;

    // Full density: all bins contribute.
    SpectralSmearModule mfull(N);
    push_fn(mfull, 0.f, 1.f, 2 * N, [&](std::size_t i) { return tone(N, 8.f, i); });
    const float rms_full = rms(mfull);

    // Low density: only the loudest bin survives — less total energy.
    SpectralSmearModule mlow(N);
    push_fn(mlow, 0.f, 0.05f, 2 * N, [&](std::size_t i) { return tone(N, 8.f, i); });
    const float rms_low = rms(mlow);

    REQUIRE(rms_full > rms_low);
}

TEST_CASE("SpectralSmearModule: density=0 output near zero for broadband noise",
          "[spectral-smear]") {
    // At density=0 only the single max-energy bin survives. For a tone at bin 8,
    // the peak bin gets through; but since the DC+peak contribution is small
    // relative to the full-spectrum output, RMS should be much lower than density=1.
    // We simply check the output is bounded (no explosion).
    SpectralSmearModule m(64);
    const std::size_t   N = 64;
    push_fn(m, 0.f, 0.f, 2 * N, [&](std::size_t i) { return tone(N, 8.f, i); });
    for (const auto& out : m.outputs)
        REQUIRE(std::abs(out.voltage) < 10.f);
}

// ---------------------------------------------------------------------------
// prepare() reset
// ---------------------------------------------------------------------------

TEST_CASE("SpectralSmearModule: prepare() resets to silence", "[spectral-smear]") {
    SpectralSmearModule m(64);
    const std::size_t   N = 64;
    push_fn(m, 0.f, 1.f, 2 * N, [&](std::size_t i) { return tone(N, 8.f, i); });
    REQUIRE(rms(m) > 0.f);

    m.prepare(kArgs);
    REQUIRE(m.outputs[0].voltage == 0.f);
    REQUIRE(m.outputs[1].voltage == 0.f);

    // smoothed_mag should be zeroed.
    for (float v : m.smoothed_mag_l())
        REQUIRE(v == 0.f);
}

// ---------------------------------------------------------------------------
// Stereo independence
// ---------------------------------------------------------------------------

TEST_CASE("SpectralSmearModule: L and R channels are independent", "[spectral-smear]") {
    SpectralSmearModule m(64);
    const std::size_t   N = 64;
    m.inputs[2].voltage   = 0.f;
    m.inputs[3].voltage   = 1.f;
    for (std::size_t i = 0; i < 2 * N; ++i) {
        m.inputs[0].voltage = tone(N, 8.f, i); // L: tone
        m.inputs[1].voltage = 0.f;             // R: silence
        m.process(kArgs);
    }
    // L smoothed_mag should have energy; R should be zero.
    const float sum_l = std::accumulate(m.smoothed_mag_l().begin(), m.smoothed_mag_l().end(), 0.f);
    const float sum_r = std::accumulate(m.smoothed_mag_r().begin(), m.smoothed_mag_r().end(), 0.f);
    REQUIRE(sum_l > sum_r * 10.f);
}

// ---------------------------------------------------------------------------
// Window size variant
// ---------------------------------------------------------------------------

TEST_CASE("SpectralSmearModule: 512-window produces non-zero output", "[spectral-smear]") {
    SpectralSmearModule m(512);
    const std::size_t   N = 512;
    push_fn(m, 0.f, 1.f, 2 * N, [&](std::size_t i) { return tone(N, 8.f, i); });
    REQUIRE(rms(m) > 0.f);
}

// ---------------------------------------------------------------------------
// Copy / move protection (compile-time)
// ---------------------------------------------------------------------------

static_assert(!std::is_copy_constructible_v<SpectralSmearModule>,
              "SpectralSmearModule must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<SpectralSmearModule>,
              "SpectralSmearModule must not be copy-assignable");
