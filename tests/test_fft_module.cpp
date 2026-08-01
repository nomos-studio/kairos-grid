// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/fft/fft_module.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace kairos_grid;
using Catch::Approx;

namespace {

// Push `n` samples of the same value to both channels and call process() n times.
// The module accumulates samples in its ring buffer and fires analysis hops
// automatically.  Call this to warm the ring to a known state.
void push(FftModule& m, float sample_l, float sample_r, std::size_t n) {
    const GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};
    for (std::size_t i = 0; i < n; ++i) {
        m.inputs[0].voltage = sample_l;
        m.inputs[1].voltage = sample_r;
        m.process(kArgs);
    }
}

// Convenience: same value for both channels.
void push(FftModule& m, float sample, std::size_t n) {
    push(m, sample, sample, n);
}

// Fill the ring with `n` samples drawn from the generator f(i), where i is the
// sample index.
void push_fn(FftModule& m, std::size_t n, auto fn) {
    const GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};
    for (std::size_t i = 0; i < n; ++i) {
        float s             = fn(i);
        m.inputs[0].voltage = s;
        m.inputs[1].voltage = s;
        m.process(kArgs);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Construction and accessors
// ---------------------------------------------------------------------------

TEST_CASE("FftModule: port counts — 2 inputs, 6 outputs", "[fft]") {
    FftModule m;
    REQUIRE(m.inputs.size() == 2);
    REQUIRE(m.outputs.size() == 6);
}

TEST_CASE("FftModule: default window is 1024, hop is 256", "[fft]") {
    FftModule m;
    REQUIRE(m.window() == 1024);
    REQUIRE(m.hop_size() == 256);
}

TEST_CASE("FftModule: custom window 512", "[fft]") {
    FftModule m(512);
    REQUIRE(m.window() == 512);
    REQUIRE(m.hop_size() == 128);
}

TEST_CASE("FftModule: mag_l / mag_r size is window/2 + 1", "[fft]") {
    FftModule m(64);
    REQUIRE(m.mag_l().size() == 33); // 64/2 + 1
    REQUIRE(m.mag_r().size() == 33);
}

// ---------------------------------------------------------------------------
// Initial state and prepare()
// ---------------------------------------------------------------------------

TEST_CASE("FftModule: outputs are 0 before any samples", "[fft]") {
    FftModule m(64);
    REQUIRE(m.outputs[0].voltage == 0.f); // centroid-l
    REQUIRE(m.outputs[1].voltage == 0.f); // centroid-r
    REQUIRE(m.outputs[2].voltage == 0.f); // flatness-l
    REQUIRE(m.outputs[3].voltage == 0.f); // flatness-r
    REQUIRE(m.outputs[4].voltage == 0.f); // flux-l
    REQUIRE(m.outputs[5].voltage == 0.f); // flux-r
}

TEST_CASE("FftModule: prepare() resets all descriptors to 0", "[fft]") {
    FftModule m(64);
    // Warm the ring so all descriptors are non-trivially set.
    push(m, 0.5f, 64);
    // Descriptors are now non-zero (flatness will be ~1.0 from eps-dominated silence,
    // centroid and flux may be non-zero).  Reset.
    const GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};
    m.prepare(kArgs);

    REQUIRE(m.outputs[0].voltage == 0.f);
    REQUIRE(m.outputs[2].voltage == 0.f);
    REQUIRE(m.outputs[4].voltage == 0.f);
}

TEST_CASE("FftModule: prepare() resets mag arrays to 0", "[fft]") {
    FftModule m(64);
    push(m, 1.f, 64);
    const GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};
    m.prepare(kArgs);
    for (float v : m.mag_l())
        REQUIRE(v == 0.f);
    for (float v : m.mag_r())
        REQUIRE(v == 0.f);
}

// ---------------------------------------------------------------------------
// Centroid
// ---------------------------------------------------------------------------

TEST_CASE("FftModule: DC input — centroid near 0", "[fft]") {
    // A constant DC signal concentrates all energy at bin 0.
    // centroid = 0 * mag[0] / mag[0] / (N/2) = 0.
    FftModule m(64);
    push(m, 1.f, 64); // fill one complete window of DC
    REQUIRE(m.outputs[0].voltage == Approx(0.f).margin(0.05f));
}

TEST_CASE("FftModule: Nyquist-band signal — centroid near 1", "[fft]") {
    // Alternating +1/-1 = cosine at Nyquist → energy in the top bin.
    FftModule m(64);
    push_fn(m, 64, [](std::size_t i) { return (i % 2 == 0) ? 1.f : -1.f; });
    REQUIRE(m.outputs[0].voltage == Approx(1.f).margin(0.1f));
}

TEST_CASE("FftModule: mid-frequency sine — centroid in expected range", "[fft]") {
    // A sine at bin k0 = 8 out of N=64 bins (N/2+1 = 33 non-negative bins).
    // Centroid ≈ k0 / (N/2) = 8/32 = 0.25.
    // With Hann window spectral leakage the centroid is approximate.
    FftModule         m(64);
    const std::size_t N  = 64;
    const float       k0 = 8.f;
    push_fn(m, N, [&](std::size_t i) {
        return std::sin(2.f * 3.14159265f * k0 / static_cast<float>(N) * static_cast<float>(i));
    });
    // Allow generous margin for Hann window spectral leakage.
    REQUIRE(m.outputs[0].voltage == Approx(k0 / (N / 2.f)).margin(0.1f));
}

// ---------------------------------------------------------------------------
// Flatness
// ---------------------------------------------------------------------------

TEST_CASE("FftModule: pure tone — flatness near 0", "[fft]") {
    // Pure tones concentrate energy in one bin; almost all other bins get
    // only Hann leakage.  Wiener entropy is low → flatness near 0.
    FftModule         m(64);
    const std::size_t N  = 64;
    const float       k0 = 8.f;
    push_fn(m, N, [&](std::size_t i) {
        return std::sin(2.f * 3.14159265f * k0 / static_cast<float>(N) * static_cast<float>(i));
    });
    REQUIRE(m.outputs[2].voltage < 0.3f);
}

TEST_CASE("FftModule: steady DC — flatness near 0 (single-bin signal)", "[fft]") {
    // DC is a single-bin signal just like a pure tone.
    FftModule m(64);
    push(m, 1.f, 64);
    REQUIRE(m.outputs[2].voltage < 0.3f);
}

TEST_CASE("FftModule: flatness in [0, 1] for all tested inputs", "[fft]") {
    FftModule         m(64);
    const std::size_t N = 64;
    // DC
    push(m, 1.f, N);
    REQUIRE(m.outputs[2].voltage >= 0.f);
    REQUIRE(m.outputs[2].voltage <= 1.f);

    // Nyquist
    const GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};
    m.prepare(kArgs);
    push_fn(m, N, [](std::size_t i) { return (i % 2 == 0) ? 1.f : -1.f; });
    REQUIRE(m.outputs[2].voltage >= 0.f);
    REQUIRE(m.outputs[2].voltage <= 1.f);
}

// ---------------------------------------------------------------------------
// Flux
// ---------------------------------------------------------------------------

TEST_CASE("FftModule: stable signal — flux near 0", "[fft]") {
    // Push one full window of DC to warm the ring.  Then push another hop of the
    // same DC — the spectrum is identical to the previous frame so flux≈0.
    FftModule m(64);
    push(m, 1.f, 64 + 16); // warm + one extra hop of same signal
    REQUIRE(m.outputs[4].voltage == Approx(0.f).margin(0.1f));
}

TEST_CASE("FftModule: onset after silence — flux near 1", "[fft]") {
    // Start from silence (prev_mag all 0), then push a full-amplitude DC burst.
    // The positive spectral difference is maximal → flux near 1.
    FftModule m(64);
    // Silence: ring zero-filled already.  Push one hop of DC.
    push(m, 1.f, 16); // first hop fires with 16 DC and 48 zeros
    // flux = 2 * dc_energy / (dc_energy + 0) ≈ 2 → clamped to 1.0
    REQUIRE(m.outputs[4].voltage > 0.5f);
}

TEST_CASE("FftModule: flux in [0, 1] always", "[fft]") {
    FftModule m(64);
    // Alternate between DC and zero every hop to produce maximum flux per hop.
    for (int rep = 0; rep < 8; ++rep) {
        float v = (rep % 2 == 0) ? 1.f : 0.f;
        push(m, v, 16);
        REQUIRE(m.outputs[4].voltage >= 0.f);
        REQUIRE(m.outputs[4].voltage <= 1.f);
    }
}

// ---------------------------------------------------------------------------
// Stereo independence
// ---------------------------------------------------------------------------

TEST_CASE("FftModule: left and right channels are independent", "[fft]") {
    // L gets DC (centroid→0), R gets Nyquist (centroid→1).
    FftModule             m(64);
    const GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};
    for (std::size_t i = 0; i < 64; ++i) {
        m.inputs[0].voltage = 1.f;                       // DC on L
        m.inputs[1].voltage = (i % 2 == 0) ? 1.f : -1.f; // Nyquist on R
        m.process(kArgs);
    }
    REQUIRE(m.outputs[0].voltage == Approx(0.f).margin(0.1f)); // centroid-l
    REQUIRE(m.outputs[1].voltage == Approx(1.f).margin(0.1f)); // centroid-r
}

TEST_CASE("FftModule: mag_l and mag_r are independent arrays", "[fft]") {
    FftModule m(64);
    // Feed DC on L, silence on R.
    push(m, 1.f, 0.f, 64);
    // L mag[0] should be significant; R mag[0] should be near 0.
    REQUIRE(m.mag_l()[0] > 0.f);
    REQUIRE(m.mag_r()[0] == Approx(0.f).margin(1e-4f));
}

// ---------------------------------------------------------------------------
// Multiple window sizes
// ---------------------------------------------------------------------------

TEST_CASE("FftModule: 512-window DC centroid near 0", "[fft]") {
    FftModule m(512);
    push(m, 1.f, 512);
    REQUIRE(m.outputs[0].voltage == Approx(0.f).margin(0.05f));
}

TEST_CASE("FftModule: 2048-window DC centroid near 0", "[fft]") {
    FftModule m(2048);
    push(m, 1.f, 2048);
    REQUIRE(m.outputs[0].voltage == Approx(0.f).margin(0.05f));
}

// ---------------------------------------------------------------------------
// :buf-id protocol — shared magnitude storage (Phase 2)
// ---------------------------------------------------------------------------

TEST_CASE("FftModule: shared-mode constructor accepts external mag buffers", "[fft][buf-id]") {
    // Simulate what the factory does: pre-allocate shared_ptrs and inject them.
    const std::size_t nb    = 64 / 2 + 1; // 33 bins for window=64
    auto              mag_l = std::make_shared<std::vector<float>>(nb, 0.f);
    auto              mag_r = std::make_shared<std::vector<float>>(nb, 0.f);

    FftModule m(64, mag_l, mag_r);
    REQUIRE(m.window() == 64);
    REQUIRE(m.mag_l().size() == nb);
    REQUIRE(m.mag_r().size() == nb);
}

TEST_CASE("FftModule: mag_l_ptr / mag_r_ptr return the injected shared_ptrs", "[fft][buf-id]") {
    const std::size_t nb    = 64 / 2 + 1;
    auto              mag_l = std::make_shared<std::vector<float>>(nb, 0.f);
    auto              mag_r = std::make_shared<std::vector<float>>(nb, 0.f);

    FftModule m(64, mag_l, mag_r);
    REQUIRE(m.mag_l_ptr() == mag_l); // same shared_ptr object
    REQUIRE(m.mag_r_ptr() == mag_r);
}

TEST_CASE("FftModule: shared mag visible to external observer after analysis hop",
          "[fft][buf-id]") {
    // An external holder of the same shared_ptr sees magnitude updates written
    // by FftModule's internal analysis hop.
    const std::size_t nb    = 64 / 2 + 1;
    auto              mag_l = std::make_shared<std::vector<float>>(nb, 0.f);
    auto              mag_r = std::make_shared<std::vector<float>>(nb, 0.f);

    FftModule m(64, mag_l, mag_r);
    // Feed DC on L, silence on R.
    push(m, 1.f, 0.f, 64);

    // The external shared_ptr holders see the live magnitude — same storage.
    REQUIRE((*mag_l)[0] > 0.f);                        // DC energy visible in L
    REQUIRE((*mag_r)[0] == Approx(0.f).margin(1e-4f)); // R stays silent
}

TEST_CASE("FftModule: standalone mag_l_ptr returns valid shared_ptr", "[fft][buf-id]") {
    // Standalone (no :buf-id) mode still exposes shared_ptrs so downstream
    // C++ code can share the mag arrays without EDN wiring.
    FftModule m(64);
    REQUIRE(m.mag_l_ptr() != nullptr);
    REQUIRE(m.mag_r_ptr() != nullptr);
    REQUIRE(m.mag_l_ptr()->size() == 64 / 2 + 1);
}

TEST_CASE("FftModule: shared storage prepare() zeroes the external buffer", "[fft][buf-id]") {
    const std::size_t     nb    = 64 / 2 + 1;
    auto                  mag_l = std::make_shared<std::vector<float>>(nb, 0.f);
    auto                  mag_r = std::make_shared<std::vector<float>>(nb, 0.f);
    FftModule             m(64, mag_l, mag_r);
    const GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};

    push(m, 1.f, 0.f, 64); // populate mag_l
    REQUIRE((*mag_l)[0] > 0.f);

    m.prepare(kArgs);
    for (float v : *mag_l)
        REQUIRE(v == 0.f); // prepare() zeroes the shared buffer
}

// ---------------------------------------------------------------------------
// Copy / move protection (static assertions, not runtime tests)
// ---------------------------------------------------------------------------

static_assert(!std::is_copy_constructible_v<FftModule>, "FftModule must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<FftModule>, "FftModule must not be copy-assignable");
