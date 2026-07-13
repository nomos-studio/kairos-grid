// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/shaper/shaper_module.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace kairos_grid;
using Catch::Approx;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};

static void prepare_and_step(ShaperModule& m, float in_l, float in_r, float drive, float shape,
                             float mix) {
    m.prepare(kArgs);
    m.inputs[0].voltage = in_l;
    m.inputs[1].voltage = in_r;
    m.inputs[2].voltage = drive;
    m.inputs[3].voltage = shape;
    m.inputs[4].voltage = mix;
    m.process(kArgs);
}

static float step(ShaperModule& m, float in_l, float drive, float shape, float mix) {
    m.inputs[0].voltage = in_l;
    m.inputs[1].voltage = in_l;
    m.inputs[2].voltage = drive;
    m.inputs[3].voltage = shape;
    m.inputs[4].voltage = mix;
    m.process(kArgs);
    return m.outputs[0].voltage;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("ShaperModule: port counts for all curves", "[shaper]") {
    REQUIRE(ShaperModule(ShaperCurve::Tanh).inputs.size() == 5);
    REQUIRE(ShaperModule(ShaperCurve::Tanh).outputs.size() == 2);
    REQUIRE(ShaperModule(ShaperCurve::Fold).inputs.size() == 5);
    REQUIRE(ShaperModule(ShaperCurve::Fold).outputs.size() == 2);
    REQUIRE(ShaperModule(ShaperCurve::Quantize).inputs.size() == 5);
    REQUIRE(ShaperModule(ShaperCurve::Quantize).outputs.size() == 2);
}

// ---------------------------------------------------------------------------
// Zero-in → zero-out (all curves, all params)
// ---------------------------------------------------------------------------

TEST_CASE("ShaperModule: zero input → zero output — tanh", "[shaper]") {
    ShaperModule m(ShaperCurve::Tanh);
    prepare_and_step(m, 0.f, 0.f, 0.5f, 0.5f, 1.f);
    REQUIRE(m.outputs[0].voltage == Approx(0.f).margin(1e-6f));
    REQUIRE(m.outputs[1].voltage == Approx(0.f).margin(1e-6f));
}

TEST_CASE("ShaperModule: zero input → zero output — fold", "[shaper]") {
    ShaperModule m(ShaperCurve::Fold);
    prepare_and_step(m, 0.f, 0.f, 0.5f, 0.5f, 1.f);
    REQUIRE(m.outputs[0].voltage == Approx(0.f).margin(1e-6f));
    REQUIRE(m.outputs[1].voltage == Approx(0.f).margin(1e-6f));
}

TEST_CASE("ShaperModule: zero input → zero output — quantize", "[shaper]") {
    ShaperModule m(ShaperCurve::Quantize);
    prepare_and_step(m, 0.f, 0.f, 0.5f, 0.5f, 1.f);
    REQUIRE(m.outputs[0].voltage == Approx(0.f).margin(1e-6f));
    REQUIRE(m.outputs[1].voltage == Approx(0.f).margin(1e-6f));
}

// ---------------------------------------------------------------------------
// Mix = 0 → dry pass-through for all curves
// ---------------------------------------------------------------------------

TEST_CASE("ShaperModule: mix=0 passes dry signal through — tanh", "[shaper]") {
    ShaperModule m(ShaperCurve::Tanh);
    prepare_and_step(m, 0.3f, -0.7f, 1.f, 1.f, 0.f);
    REQUIRE(m.outputs[0].voltage == Approx(0.3f));
    REQUIRE(m.outputs[1].voltage == Approx(-0.7f));
}

TEST_CASE("ShaperModule: mix=0 passes dry signal through — fold", "[shaper]") {
    ShaperModule m(ShaperCurve::Fold);
    prepare_and_step(m, 0.3f, -0.7f, 1.f, 1.f, 0.f);
    REQUIRE(m.outputs[0].voltage == Approx(0.3f));
    REQUIRE(m.outputs[1].voltage == Approx(-0.7f));
}

TEST_CASE("ShaperModule: mix=0 passes dry signal through — quantize", "[shaper]") {
    ShaperModule m(ShaperCurve::Quantize);
    prepare_and_step(m, 0.3f, -0.7f, 1.f, 1.f, 0.f);
    REQUIRE(m.outputs[0].voltage == Approx(0.3f));
    REQUIRE(m.outputs[1].voltage == Approx(-0.7f));
}

// ---------------------------------------------------------------------------
// Tanh saturation behaviour
// ---------------------------------------------------------------------------

TEST_CASE("ShaperModule: tanh at high drive stays within ±1", "[shaper]") {
    ShaperModule m(ShaperCurve::Tanh);
    m.prepare(kArgs);
    for (float x : {0.9f, -0.9f, 0.5f, -0.5f, 0.1f, -0.1f}) {
        step(m, x, 1.f, 0.f, 1.f);
        REQUIRE(m.outputs[0].voltage >= -1.f - 1e-5f);
        REQUIRE(m.outputs[0].voltage <= 1.f + 1e-5f);
    }
}

TEST_CASE("ShaperModule: tanh is monotone (odd symmetry)", "[shaper]") {
    // For symmetric tanh (shape=0), f(x) and f(-x) must sum to zero.
    ShaperModule m(ShaperCurve::Tanh);
    m.prepare(kArgs);
    for (float x : {0.1f, 0.3f, 0.6f, 0.9f}) {
        const float pos = step(m, x, 0.5f, 0.f, 1.f);
        const float neg = step(m, -x, 0.5f, 0.f, 1.f);
        // ADAA prev state means consecutive samples affect each other, so reset
        m.prepare(kArgs);
        const float p2 = step(m, x, 0.5f, 0.f, 1.f);
        m.prepare(kArgs);
        const float n2 = step(m, -x, 0.5f, 0.f, 1.f);
        REQUIRE(p2 == Approx(-n2).margin(1e-5f));
        (void)pos;
        (void)neg;
    }
}

TEST_CASE("ShaperModule: tanh shape=1 produces hard clip", "[shaper]") {
    // At shape=1 the curve morphs fully to hard clip; signal > 1V clamps to 1.
    ShaperModule m(ShaperCurve::Tanh);
    m.prepare(kArgs);
    // Two consecutive samples both > 1 (after gain); ADAA convergence.
    step(m, 0.9f, 1.f, 1.f, 1.f); // warm up ADAA prev
    step(m, 0.9f, 1.f, 1.f, 1.f);
    // At drive=1 gain=16×, 0.9V → 14.4V pre-shape; hard-clip at shape=1 → ±1.
    REQUIRE(m.outputs[0].voltage == Approx(1.f).margin(1e-4f));
}

TEST_CASE("ShaperModule: tanh has increasing saturation with drive", "[shaper]") {
    // A larger drive value should push the output closer to the saturation ceiling.
    ShaperModule m(ShaperCurve::Tanh);
    m.prepare(kArgs);
    const float out_low = step(m, 0.5f, 0.1f, 0.f, 1.f);
    m.prepare(kArgs);
    const float out_high = step(m, 0.5f, 0.9f, 0.f, 1.f);
    REQUIRE(out_high > out_low);
}

// ---------------------------------------------------------------------------
// Fold behaviour
// ---------------------------------------------------------------------------

TEST_CASE("ShaperModule: fold output stays within ±1 at high drive+shape", "[shaper]") {
    ShaperModule m(ShaperCurve::Fold);
    m.prepare(kArgs);
    for (float x : {0.5f, -0.5f, 0.8f, -0.8f}) {
        step(m, x, 1.f, 1.f, 1.f);
        REQUIRE(m.outputs[0].voltage >= -1.f - 1e-5f);
        REQUIRE(m.outputs[0].voltage <= 1.f + 1e-5f);
    }
}

TEST_CASE("ShaperModule: fold at low drive+shape ≈ tanh near origin", "[shaper]") {
    // Near the origin sin(π/2·x) ≈ π/2·x for small x; with unity gain the fold
    // barely changes amplitude.  After ADAA warmup, result should be close to input.
    ShaperModule m(ShaperCurve::Fold);
    m.prepare(kArgs);
    // Two samples at the same tiny value so ADAA prev = current → direct evaluation.
    step(m, 0.05f, 0.f, 0.f, 1.f);
    const float out = step(m, 0.05f, 0.f, 0.f, 1.f);
    // sin(π/2 · 0.05) ≈ 0.0785 ≈ 0.05 * (π/2) — close to input scaled by π/2.
    const float expected = std::sin(1.5707963f * 0.05f);
    REQUIRE(out == Approx(expected).margin(1e-4f));
}

TEST_CASE("ShaperModule: fold shape changes steady-state output", "[shaper]") {
    // At steady state (two identical consecutive samples), ADAA falls back to direct
    // f_sample evaluation.  shape bakes into the pre-fold gain so different shape
    // values place the signal at different points in the sine fold domain.
    //
    // drive=0.5 → gain≈4.  shape=0 → fold_scale=1 → x=1.6 → sin(0.8π)≈+0.588.
    //                        shape=1 → fold_scale=4 → x=6.4 → sin(3.2π)≈-0.588.
    ShaperModule m(ShaperCurve::Fold);

    m.prepare(kArgs);
    step(m, 0.4f, 0.5f, 0.f, 1.f);                          // warm up prev_l_ = 1.6
    const float out_gentle = step(m, 0.4f, 0.5f, 0.f, 1.f); // ADAA fallback → sin(0.8π)

    m.prepare(kArgs);
    step(m, 0.4f, 0.5f, 1.f, 1.f);                        // warm up prev_l_ = 6.4
    const float out_deep = step(m, 0.4f, 0.5f, 1.f, 1.f); // ADAA fallback → sin(3.2π)

    // drive=0.5 → gain≈4; x_gentle=1.6 → sin(π/2·1.6)≈0.588; x_deep=6.4 → sin(π/2·6.4)≈-0.588.
    REQUIRE(out_gentle == Approx(std::sin(1.5707963f * 1.6f)).margin(1e-4f));
    REQUIRE(out_deep == Approx(std::sin(1.5707963f * 6.4f)).margin(1e-4f));
    // The two fold positions are on opposite sides of a fold reflection.
    REQUIRE(out_gentle > 0.5f);
    REQUIRE(out_deep < -0.5f);
}

// ---------------------------------------------------------------------------
// Quantize behaviour
// ---------------------------------------------------------------------------

TEST_CASE("ShaperModule: quantize output is exactly on discrete levels", "[shaper]") {
    ShaperModule m(ShaperCurve::Quantize);
    m.prepare(kArgs);
    // shape=1 → 2-bit (4 levels: ±0.25, ±0.75 at mid-point, etc.)
    // levels = pow(2, 12 - 10) = 4
    const float levels = std::pow(2.f, 12.f - 1.f * 10.f); // 4.0
    for (float x : {0.1f, 0.33f, 0.66f, -0.5f, -0.9f}) {
        step(m, x, 0.f, 1.f, 1.f);
        const float out     = m.outputs[0].voltage;
        const float rounded = std::round(out * levels) / levels;
        REQUIRE(out == Approx(rounded).margin(1e-6f));
    }
}

TEST_CASE("ShaperModule: quantize shape=0 is near-clean (12-bit)", "[shaper]") {
    // At shape=0: 4096 levels → quantization step = 1/4096 ≈ 0.000244
    ShaperModule m(ShaperCurve::Quantize);
    m.prepare(kArgs);
    const float in = 0.5f;
    step(m, in, 0.f, 0.f, 1.f);
    REQUIRE(m.outputs[0].voltage == Approx(in).margin(2.f / 4096.f));
}

TEST_CASE("ShaperModule: quantize drive clips before reducing", "[shaper]") {
    // At drive=1 gain=16×; input 0.5V → 8V → clipped to 1V before quantize.
    ShaperModule m(ShaperCurve::Quantize);
    m.prepare(kArgs);
    step(m, 0.5f, 1.f, 0.f, 1.f);
    // Output should be ≈1 (saturated), not ≈8.
    REQUIRE(m.outputs[0].voltage <= 1.f + 1e-5f);
    REQUIRE(m.outputs[0].voltage > 0.9f);
}

// ---------------------------------------------------------------------------
// Stereo independence
// ---------------------------------------------------------------------------

TEST_CASE("ShaperModule: L and R channels processed independently — tanh", "[shaper]") {
    ShaperModule m(ShaperCurve::Tanh);
    prepare_and_step(m, 0.3f, -0.7f, 0.5f, 0.3f, 1.f);
    // Outputs should be different (different input amplitudes).
    REQUIRE(m.outputs[0].voltage != Approx(m.outputs[1].voltage).margin(0.01f));
    // Each output must be bounded.
    REQUIRE(std::abs(m.outputs[0].voltage) <= 1.f + 1e-4f);
    REQUIRE(std::abs(m.outputs[1].voltage) <= 1.f + 1e-4f);
}

TEST_CASE("ShaperModule: L and R channels processed independently — quantize", "[shaper]") {
    ShaperModule m(ShaperCurve::Quantize);
    prepare_and_step(m, 0.3f, -0.7f, 0.f, 0.5f, 1.f);
    REQUIRE(m.outputs[0].voltage != Approx(m.outputs[1].voltage).margin(0.01f));
}

// ---------------------------------------------------------------------------
// ADAA smoothness: steady-state consecutive samples should not glitch
// ---------------------------------------------------------------------------

TEST_CASE("ShaperModule: ADAA tanh steady state matches direct evaluation", "[shaper]") {
    // When prev == current, ADAA falls back to direct f(x).  Feed the same sample
    // twice; the second should equal tanh applied to x * gain.
    ShaperModule m(ShaperCurve::Tanh);
    m.prepare(kArgs);
    const float x    = 0.4f;
    const float gain = std::exp(0.3f * 2.772588722f); // drive=0.3 → exp(0.3*ln16)
    step(m, x, 0.3f, 0.f, 1.f);                       // warm up prev
    const float out = step(m, x, 0.3f, 0.f, 1.f);     // same sample → ADAA fallback
    REQUIRE(out == Approx(std::tanh(x * gain)).margin(1e-4f));
}

// ---------------------------------------------------------------------------
// Finite output guarantee across all curves
// ---------------------------------------------------------------------------

TEST_CASE("ShaperModule: finite outputs for swept inputs — tanh", "[shaper]") {
    ShaperModule m(ShaperCurve::Tanh);
    m.prepare(kArgs);
    for (int i = -100; i <= 100; ++i) {
        const float x = static_cast<float>(i) * 0.01f;
        step(m, x, 0.8f, 0.5f, 1.f);
        REQUIRE(std::isfinite(m.outputs[0].voltage));
    }
}

TEST_CASE("ShaperModule: finite outputs for swept inputs — fold", "[shaper]") {
    ShaperModule m(ShaperCurve::Fold);
    m.prepare(kArgs);
    for (int i = -100; i <= 100; ++i) {
        const float x = static_cast<float>(i) * 0.01f;
        step(m, x, 0.8f, 0.5f, 1.f);
        REQUIRE(std::isfinite(m.outputs[0].voltage));
    }
}

TEST_CASE("ShaperModule: finite outputs for swept inputs — quantize", "[shaper]") {
    ShaperModule m(ShaperCurve::Quantize);
    m.prepare(kArgs);
    for (int i = -100; i <= 100; ++i) {
        const float x = static_cast<float>(i) * 0.01f;
        step(m, x, 0.8f, 0.5f, 1.f);
        REQUIRE(std::isfinite(m.outputs[0].voltage));
    }
}
