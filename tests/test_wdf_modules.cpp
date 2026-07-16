// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/wdf/wdf_modules.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numeric>
#include <vector>

using namespace kairos_grid;
using Catch::Approx;

namespace {

const GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};

// Push n samples through a module and collect outputs.
template <typename Mod>
std::vector<float> run(Mod& m, std::vector<float> const& in, float drive = 1.f) {
    std::vector<float> out;
    out.reserve(in.size());
    for (float s : in) {
        m.inputs[0].voltage = s;
        m.inputs[1].voltage = drive;
        m.process(kArgs);
        out.push_back(m.outputs[0].voltage);
    }
    return out;
}

// Generate n samples of a sine at freq Hz / fs.
std::vector<float> sine(int n, float freq = 440.f, float fs = 48000.f, float amp = 1.f) {
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = amp * std::sin(2.f * 3.14159265f * freq / fs * float(i));
    return v;
}

float rms(std::vector<float> const& v) {
    if (v.empty())
        return 0.f;
    float s = std::inner_product(v.begin(), v.end(), v.begin(), 0.f);
    return std::sqrt(s / float(v.size()));
}

} // namespace

// ---------------------------------------------------------------------------
// DiodeClipModule
// ---------------------------------------------------------------------------

TEST_CASE("DiodeClipModule: port counts — 2 inputs, 1 output", "[wdf]") {
    DiodeClipModule m;
    REQUIRE(m.inputs.size() == 2);
    REQUIRE(m.outputs.size() == 1);
}

TEST_CASE("DiodeClipModule: outputs 0 before any samples", "[wdf]") {
    DiodeClipModule m;
    REQUIRE(m.outputs[0].voltage == 0.f);
}

TEST_CASE("DiodeClipModule: prepare() resets output to 0", "[wdf]") {
    DiodeClipModule m;
    m.prepare(kArgs);
    auto sig = sine(48, 440.f);
    run(m, sig);
    m.prepare(kArgs);
    REQUIRE(m.outputs[0].voltage == 0.f);
}

TEST_CASE("DiodeClipModule: clips hard-driven signal below input amplitude", "[wdf]") {
    // A 1N4148 pair clips at ~±0.4V.  Feed a 2V amplitude sine at drive=2;
    // effective input = 4V.  RMS of output should be well below RMS of input.
    DiodeClipModule m;
    m.prepare(kArgs);

    // Warm transient
    auto warm = sine(4800, 440.f, 48000.f, 2.f);
    run(m, warm, 2.f);

    auto sig = sine(4800, 440.f, 48000.f, 2.f);
    auto out = run(m, sig, 2.f);

    const float in_rms  = rms(sig) * 2.f; // actual input after drive
    const float out_rms = rms(out);
    REQUIRE(out_rms < in_rms * 0.5f); // significant gain reduction from clipping
}

TEST_CASE("DiodeClipModule: zero drive produces near-silence", "[wdf]") {
    DiodeClipModule m;
    m.prepare(kArgs);

    auto sig = sine(4800, 440.f, 48000.f, 1.f);
    auto out = run(m, sig, 0.f); // drive=0 → Vs.setVoltage(0)

    REQUIRE(rms(out) < 1e-4f);
}

TEST_CASE("DiodeClipModule: symmetric — equal RMS on positive/negative half-cycles", "[wdf]") {
    // Antiparallel pair is symmetric: flipping the input sign should produce the
    // same RMS output.
    DiodeClipModule m;
    m.prepare(kArgs);
    auto sig = sine(4800, 440.f, 48000.f, 2.f);

    auto out_pos = run(m, sig, 2.f);
    m.prepare(kArgs);

    std::vector<float> sig_neg(sig.size());
    for (std::size_t i = 0; i < sig.size(); ++i)
        sig_neg[i] = -sig[i];
    auto out_neg = run(m, sig_neg, 2.f);

    REQUIRE(rms(out_pos) == Approx(rms(out_neg)).margin(0.01f));
}

TEST_CASE("DiodeClipModule: output bounded — no runaway values", "[wdf]") {
    DiodeClipModule m;
    m.prepare(kArgs);

    auto sig = sine(48000, 440.f, 48000.f, 10.f); // extreme amplitude
    auto out = run(m, sig, 5.f);

    for (float v : out)
        REQUIRE(std::abs(v) < 2.f); // physically bounded by diode forward voltage
}

TEST_CASE("DiodeClipModule: non-copyable", "[wdf]") {
    static_assert(!std::is_copy_constructible_v<DiodeClipModule>);
    static_assert(!std::is_copy_assignable_v<DiodeClipModule>);
}

// ---------------------------------------------------------------------------
// DiodeHalfModule
// ---------------------------------------------------------------------------

TEST_CASE("DiodeHalfModule: port counts — 2 inputs, 1 output", "[wdf]") {
    DiodeHalfModule m;
    REQUIRE(m.inputs.size() == 2);
    REQUIRE(m.outputs.size() == 1);
}

TEST_CASE("DiodeHalfModule: prepare() resets output to 0", "[wdf]") {
    DiodeHalfModule m;
    m.prepare(kArgs);
    auto sig = sine(48, 440.f);
    run(m, sig);
    m.prepare(kArgs);
    REQUIRE(m.outputs[0].voltage == 0.f);
}

TEST_CASE("DiodeHalfModule: asymmetric — non-zero DC offset in steady state", "[wdf]") {
    // A single diode produces asymmetric output: one half-cycle clips while the
    // other passes through the RC shunt.  The rectifying action produces a net DC
    // bias in steady state.  An antiparallel pair (DiodeClipModule) has zero DC
    // bias by symmetry.
    DiodeHalfModule m_half;
    DiodeClipModule m_clip;
    m_half.prepare(kArgs);
    m_clip.prepare(kArgs);

    // Warm both modules to steady state.
    auto warm = sine(4800, 440.f, 48000.f, 2.f);
    run(m_half, warm, 2.f);
    run(m_clip, warm, 2.f);
    m_half.prepare(kArgs);
    m_clip.prepare(kArgs);

    auto sig      = sine(9600, 440.f, 48000.f, 2.f);
    auto out_half = run(m_half, sig, 2.f);
    auto out_clip = run(m_clip, sig, 2.f);

    // Mean of DiodeHalf output should be measurably non-zero.
    float mean_half =
        std::accumulate(out_half.begin(), out_half.end(), 0.f) / float(out_half.size());
    float mean_clip =
        std::accumulate(out_clip.begin(), out_clip.end(), 0.f) / float(out_clip.size());

    REQUIRE(std::abs(mean_half) > std::abs(mean_clip) + 0.01f);
}

TEST_CASE("DiodeHalfModule: clips driven signal", "[wdf]") {
    DiodeHalfModule m;
    m.prepare(kArgs);

    auto warm = sine(4800, 440.f, 48000.f, 2.f);
    run(m, warm, 2.f);

    auto sig = sine(4800, 440.f, 48000.f, 2.f);
    auto out = run(m, sig, 2.f);

    REQUIRE(rms(out) < rms(sig) * 2.f * 0.8f);
}

TEST_CASE("DiodeHalfModule: output numerically stable at extreme input", "[wdf]") {
    // Verify no NaN/Inf at very high drive. The unblocked half-cycle charges the
    // RC network to near-source amplitude, so a fixed voltage bound would be
    // drive-dependent; numerical stability is the meaningful invariant here.
    DiodeHalfModule m;
    m.prepare(kArgs);

    auto sig = sine(48000, 440.f, 48000.f, 10.f);
    auto out = run(m, sig, 5.f);

    for (float v : out)
        REQUIRE(std::isfinite(v));
}

TEST_CASE("DiodeHalfModule: non-copyable", "[wdf]") {
    static_assert(!std::is_copy_constructible_v<DiodeHalfModule>);
    static_assert(!std::is_copy_assignable_v<DiodeHalfModule>);
}

// ---------------------------------------------------------------------------
// Cross-module: same input, different character
// ---------------------------------------------------------------------------

TEST_CASE("WDF modules: diode-clip clips harder than diode-half at same drive", "[wdf]") {
    // Antiparallel pair clips both half-cycles (output ≈ ±0.4V); single diode
    // clips only one direction while the other passes through the RC shunt at
    // near-full amplitude.  Net result: diode-clip RMS < diode-half RMS.
    DiodeClipModule clip;
    DiodeHalfModule half;
    clip.prepare(kArgs);
    half.prepare(kArgs);

    auto warm = sine(4800, 440.f, 48000.f, 2.f);
    run(clip, warm, 2.f);
    run(half, warm, 2.f);
    clip.prepare(kArgs);
    half.prepare(kArgs);

    auto sig      = sine(4800, 440.f, 48000.f, 2.f);
    auto out_clip = run(clip, sig, 2.f);
    auto out_half = run(half, sig, 2.f);

    REQUIRE(rms(out_clip) < rms(out_half));
}
