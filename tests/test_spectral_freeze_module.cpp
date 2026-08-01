// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/fft/spectral_freeze_module.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace kairos_grid;
using Catch::Approx;

namespace {

static constexpr float kPi    = 3.14159265f;
static constexpr float kTwoPi = 6.28318530f;
const GridProcessArgs  kArgs{48000.f, 1.f / 48000.f, 0};

// Push n samples; freeze gate driven by the freeze_v argument.
void push(SpectralFreezeModule& m, float in_l, float in_r, float freeze_v, std::size_t n) {
    m.inputs[2].voltage = freeze_v;
    for (std::size_t i = 0; i < n; ++i) {
        m.inputs[0].voltage = in_l;
        m.inputs[1].voltage = in_r;
        m.process(kArgs);
    }
}

// Convenience: same value both channels, controllable freeze gate.
void push(SpectralFreezeModule& m, float sample, float freeze_v, std::size_t n) {
    push(m, sample, sample, freeze_v, n);
}

// Push n samples driven by a generator; freeze gate held at freeze_v.
void push_fn(SpectralFreezeModule& m, float freeze_v, std::size_t n, auto fn) {
    m.inputs[2].voltage = freeze_v;
    for (std::size_t i = 0; i < n; ++i) {
        const float s       = fn(i);
        m.inputs[0].voltage = s;
        m.inputs[1].voltage = s;
        m.process(kArgs);
    }
}

// Raise freeze gate for one sample, then hold it high for n_hold more.
void trigger_and_hold(SpectralFreezeModule& m, float in_l, float in_r, std::size_t n_hold) {
    // Rising edge
    m.inputs[2].voltage = 1.f;
    m.inputs[0].voltage = in_l;
    m.inputs[1].voltage = in_r;
    m.process(kArgs);
    // Hold high
    push(m, in_l, in_r, 1.f, n_hold);
}

// Return true if any output is non-zero.
bool any_nonzero(const SpectralFreezeModule& m) {
    for (const auto& out : m.outputs)
        if (out.voltage != 0.f)
            return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction and accessors
// ---------------------------------------------------------------------------

TEST_CASE("SpectralFreezeModule: port counts — 3 inputs, 2 outputs", "[spectral-freeze]") {
    SpectralFreezeModule m;
    REQUIRE(m.inputs.size() == 3);
    REQUIRE(m.outputs.size() == 2);
}

TEST_CASE("SpectralFreezeModule: default window is 1024, hop is 256", "[spectral-freeze]") {
    SpectralFreezeModule m;
    REQUIRE(m.window() == 1024);
    REQUIRE(m.hop_size() == 256);
}

TEST_CASE("SpectralFreezeModule: custom window 512", "[spectral-freeze]") {
    SpectralFreezeModule m(512);
    REQUIRE(m.window() == 512);
    REQUIRE(m.hop_size() == 128);
}

TEST_CASE("SpectralFreezeModule: custom window 2048", "[spectral-freeze]") {
    SpectralFreezeModule m(2048);
    REQUIRE(m.window() == 2048);
    REQUIRE(m.hop_size() == 512);
}

// ---------------------------------------------------------------------------
// Initial state and silence without freeze
// ---------------------------------------------------------------------------

TEST_CASE("SpectralFreezeModule: outputs silent before any samples", "[spectral-freeze]") {
    SpectralFreezeModule m(64);
    REQUIRE(m.outputs[0].voltage == 0.f);
    REQUIRE(m.outputs[1].voltage == 0.f);
}

TEST_CASE("SpectralFreezeModule: outputs silent without freeze gate", "[spectral-freeze]") {
    SpectralFreezeModule m(64);
    // Warm the analysis ring with a strong tone — no freeze gate.
    push_fn(m, 0.f, 128,
            [](std::size_t i) { return std::sin(kTwoPi * 8.f / 64.f * static_cast<float>(i)); });
    REQUIRE(m.outputs[0].voltage == 0.f);
    REQUIRE(m.outputs[1].voltage == 0.f);
}

// ---------------------------------------------------------------------------
// Freeze behaviour
// ---------------------------------------------------------------------------

TEST_CASE("SpectralFreezeModule: output non-zero after freeze", "[spectral-freeze]") {
    SpectralFreezeModule m(64);
    const std::size_t    N = 64, H = 16;

    // Fill analysis ring with a tone to give the frozen spectrum energy.
    push_fn(m, 0.f, N, [&](std::size_t i) {
        return std::sin(kTwoPi * 8.f / static_cast<float>(N) * static_cast<float>(i));
    });

    // Freeze and hold for long enough that at least 4 synthesis hops fire.
    trigger_and_hold(m, 0.f, 0.f, N + H);

    REQUIRE(any_nonzero(m));
}

TEST_CASE("SpectralFreezeModule: frozen output persists across many hops", "[spectral-freeze]") {
    SpectralFreezeModule m(64);
    const std::size_t    N = 64;

    // Warm analysis with a tone.
    push_fn(m, 0.f, N, [&](std::size_t i) {
        return std::sin(kTwoPi * 8.f / static_cast<float>(N) * static_cast<float>(i));
    });

    // Freeze and hold for 8 full windows worth of samples.
    push(m, 0.f, 1.f, 8 * N);

    // After many synthesis hops, output should still be non-zero.
    REQUIRE(any_nonzero(m));
}

TEST_CASE("SpectralFreezeModule: unfreeze returns outputs to silence", "[spectral-freeze]") {
    SpectralFreezeModule m(64);
    const std::size_t    N = 64, H = 16;

    push_fn(m, 0.f, N, [&](std::size_t i) {
        return std::sin(kTwoPi * 8.f / static_cast<float>(N) * static_cast<float>(i));
    });
    // Freeze long enough to build up synthesis output.
    trigger_and_hold(m, 0.f, 0.f, N + H);
    REQUIRE(any_nonzero(m));

    // Release gate — outputs should immediately go silent.
    push(m, 0.f, 0.f, 4);
    REQUIRE(m.outputs[0].voltage == 0.f);
    REQUIRE(m.outputs[1].voltage == 0.f);
}

TEST_CASE("SpectralFreezeModule: silent spectrum → frozen output near zero", "[spectral-freeze]") {
    // If the analysis ring was full of zeros before freeze, there is no spectral
    // energy to resynthesize; output should remain at or very near zero.
    SpectralFreezeModule m(64);
    const std::size_t    N = 64, H = 16;

    // No warm-up: ring is all zeros.
    trigger_and_hold(m, 0.f, 0.f, N + H);

    for (const auto& out : m.outputs)
        REQUIRE(out.voltage == Approx(0.f).margin(1e-5f));
}

// ---------------------------------------------------------------------------
// Re-trigger
// ---------------------------------------------------------------------------

TEST_CASE("SpectralFreezeModule: re-trigger latches new spectrum", "[spectral-freeze]") {
    // First freeze on a zero-ring (output ≈ 0).  Then warm the ring with a tone,
    // re-trigger, and verify output becomes non-zero.
    SpectralFreezeModule m(64);
    const std::size_t    N = 64, H = 16;

    // First freeze — silent spectrum.
    trigger_and_hold(m, 0.f, 0.f, N + H);
    float first_freeze_max = 0.f;
    for (const auto& out : m.outputs)
        first_freeze_max = std::max(first_freeze_max, std::abs(out.voltage));

    // Release, warm, re-freeze.
    push(m, 0.f, 0.f, 4); // unfreeze
    push_fn(m, 0.f, N, [&](std::size_t i) {
        return std::sin(kTwoPi * 8.f / static_cast<float>(N) * static_cast<float>(i));
    });
    trigger_and_hold(m, 0.f, 0.f, N + H);

    float second_freeze_max = 0.f;
    for (const auto& out : m.outputs)
        second_freeze_max = std::max(second_freeze_max, std::abs(out.voltage));

    REQUIRE(second_freeze_max > first_freeze_max);
}

// ---------------------------------------------------------------------------
// prepare() reset
// ---------------------------------------------------------------------------

TEST_CASE("SpectralFreezeModule: prepare() resets all state to silent", "[spectral-freeze]") {
    SpectralFreezeModule m(64);
    const std::size_t    N = 64, H = 16;

    push_fn(m, 0.f, N, [&](std::size_t i) {
        return std::sin(kTwoPi * 8.f / static_cast<float>(N) * static_cast<float>(i));
    });
    trigger_and_hold(m, 0.f, 0.f, N + H);
    REQUIRE(any_nonzero(m));

    m.prepare(kArgs);
    REQUIRE(m.outputs[0].voltage == 0.f);
    REQUIRE(m.outputs[1].voltage == 0.f);

    // After prepare, gate should be seen as fresh (no lingering frozen state).
    push(m, 0.f, 0.f, 4);
    REQUIRE(m.outputs[0].voltage == 0.f);
}

// ---------------------------------------------------------------------------
// Stereo independence
// ---------------------------------------------------------------------------

TEST_CASE("SpectralFreezeModule: left and right channels are independent", "[spectral-freeze]") {
    // Warm L with a tone, R with silence.  After freeze, L output should have
    // significantly more energy than R.
    SpectralFreezeModule  m(64);
    const std::size_t     N = 64, H = 16;
    const GridProcessArgs kA{48000.f, 1.f / 48000.f, 0};

    for (std::size_t i = 0; i < N; ++i) {
        const float tone = std::sin(kTwoPi * 8.f / static_cast<float>(N) * static_cast<float>(i));
        m.inputs[0].voltage = tone; // L: tone
        m.inputs[1].voltage = 0.f;  // R: silence
        m.inputs[2].voltage = 0.f;
        m.process(kA);
    }

    // Freeze and run long enough for synthesis to build up.
    push(m, 0.f, 0.f, 1.f, N + H);

    float l_energy = 0.f, r_energy = 0.f;
    for (std::size_t i = 0; i < 2 * H; ++i) {
        m.inputs[0].voltage = 0.f;
        m.inputs[1].voltage = 0.f;
        m.inputs[2].voltage = 1.f;
        m.process(kA);
        l_energy += m.outputs[0].voltage * m.outputs[0].voltage;
        r_energy += m.outputs[1].voltage * m.outputs[1].voltage;
    }

    // L should have substantially more energy than R.
    REQUIRE(l_energy > r_energy * 4.f);
}

// ---------------------------------------------------------------------------
// Window size variants
// ---------------------------------------------------------------------------

TEST_CASE("SpectralFreezeModule: 512-window produces non-zero output after freeze",
          "[spectral-freeze]") {
    SpectralFreezeModule m(512);
    const std::size_t    N = 512, H = 128;

    push_fn(m, 0.f, N, [&](std::size_t i) {
        return std::sin(kTwoPi * 8.f / static_cast<float>(N) * static_cast<float>(i));
    });
    trigger_and_hold(m, 0.f, 0.f, N + H);
    REQUIRE(any_nonzero(m));
}

// ---------------------------------------------------------------------------
// Copy / move protection (compile-time)
// ---------------------------------------------------------------------------

static_assert(!std::is_copy_constructible_v<SpectralFreezeModule>,
              "SpectralFreezeModule must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<SpectralFreezeModule>,
              "SpectralFreezeModule must not be copy-assignable");
