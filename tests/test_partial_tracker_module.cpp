// SPDX-License-Identifier: GPL-3.0-or-later
#include <kairos_grid/fft/partial_tracker_module.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace kairos_grid;
using Catch::Approx;

namespace {

const GridProcessArgs kArgs{48000.f, 1.f / 48000.f, 0};
constexpr std::size_t kN = PartialTrackerModule::kMaxPeaks;
constexpr std::size_t kM = PartialTrackerModule::kMaxPartials;

// Set one peak in slot i_peak, clear all others.
void set_peak(PartialTrackerModule& m, std::size_t i_peak, float freq, float amp) {
    for (std::size_t i = 0; i < kN; ++i) {
        m.inputs[i].voltage      = 0.f;
        m.inputs[kN + i].voltage = 0.f;
    }
    m.inputs[i_peak].voltage      = freq;
    m.inputs[kN + i_peak].voltage = amp;
}

// Set two peaks.
void set_two_peaks(PartialTrackerModule& m, float f0, float a0, float f1, float a1) {
    for (std::size_t i = 0; i < kN; ++i) {
        m.inputs[i].voltage      = 0.f;
        m.inputs[kN + i].voltage = 0.f;
    }
    m.inputs[0].voltage      = f0;
    m.inputs[kN].voltage     = a0;
    m.inputs[1].voltage      = f1;
    m.inputs[kN + 1].voltage = a1;
}

// Send a one-sample trigger and then a falling-edge tick so prev_trig_ resets
// to false before any subsequent fire_trigger call.
void fire_trigger(PartialTrackerModule& m) {
    m.inputs[2 * kN].voltage = 1.f;
    m.process(kArgs);
    m.inputs[2 * kN].voltage = 0.f;
    m.process(kArgs); // register the falling edge
}

// Advance n samples without trigger.
void tick(PartialTrackerModule& m, std::size_t n) {
    m.inputs[2 * kN].voltage = 0.f;
    for (std::size_t i = 0; i < n; ++i)
        m.process(kArgs);
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("PartialTrackerModule: port counts — 18 inputs, 16 outputs", "[partial-tracker]") {
    PartialTrackerModule m;
    REQUIRE(m.inputs.size() == 2 * kN + 2); // 18
    REQUIRE(m.outputs.size() == 2 * kM);    // 16
}

// ---------------------------------------------------------------------------
// Initial state
// ---------------------------------------------------------------------------

TEST_CASE("PartialTrackerModule: all outputs zero before trigger", "[partial-tracker]") {
    PartialTrackerModule m;
    tick(m, 64);
    for (const auto& out : m.outputs)
        REQUIRE(out.voltage == 0.f);
}

// ---------------------------------------------------------------------------
// Voice birth
// ---------------------------------------------------------------------------

TEST_CASE("PartialTrackerModule: trigger with peak — voice assigned, immediate snap with smooth=0",
          "[partial-tracker]") {
    // With smooth=0 (beta=1), the voice frequency snaps to target on the same
    // trigger sample (birth sets smooth_freq = target_freq, no delta).
    PartialTrackerModule m;
    m.inputs[2 * kN + 1].voltage = 0.f; // smooth = 0
    set_peak(m, 0, 0.3f, 0.8f);

    // Pre-trigger: no output.
    tick(m, 4);
    REQUIRE(m.outputs[0].voltage == 0.f);

    // Trigger: voice 0 is born at freq=0.3, amp starts rising from 0.
    fire_trigger(m);

    // Immediately after: freq at 0.3, amp has snapped via one beta=1 step from 0 to 0.8.
    REQUIRE(m.outputs[0].voltage == Approx(0.3f).margin(1e-5f));
    REQUIRE(m.outputs[kM].voltage == Approx(0.8f).margin(1e-5f));
}

TEST_CASE("PartialTrackerModule: two simultaneous peaks — two voices assigned",
          "[partial-tracker]") {
    PartialTrackerModule m;
    m.inputs[2 * kN + 1].voltage = 0.f; // smooth = 0
    set_two_peaks(m, 0.2f, 0.9f, 0.6f, 0.5f);
    fire_trigger(m);

    // Both voices should have non-zero amplitudes.
    float total_amp = 0.f;
    for (std::size_t j = 0; j < kM; ++j)
        total_amp += m.outputs[kM + j].voltage;
    REQUIRE(total_amp > 0.f);
    // At least 2 voices assigned.
    std::size_t n_active = 0;
    for (std::size_t j = 0; j < kM; ++j)
        if (m.outputs[kM + j].voltage > 0.01f)
            ++n_active;
    REQUIRE(n_active >= 2);
}

// ---------------------------------------------------------------------------
// Voice hold and update
// ---------------------------------------------------------------------------

TEST_CASE("PartialTrackerModule: voice holds freq/amp between triggers (smooth=0)",
          "[partial-tracker]") {
    PartialTrackerModule m;
    m.inputs[2 * kN + 1].voltage = 0.f;
    set_peak(m, 0, 0.4f, 0.7f);
    fire_trigger(m);

    const float freq_after = m.outputs[0].voltage;
    const float amp_after  = m.outputs[kM].voltage;

    // 32 samples with no trigger — smooth=0 means targets don't change, outputs hold.
    tick(m, 32);
    REQUIRE(m.outputs[0].voltage == Approx(freq_after).margin(1e-5f));
    REQUIRE(m.outputs[kM].voltage == Approx(amp_after).margin(1e-5f));
}

TEST_CASE("PartialTrackerModule: second trigger updates voice to new peak within threshold",
          "[partial-tracker]") {
    // Voice is at 0.3; new peak at 0.32 (delta 0.02 < kFreqMatchThreshold=0.1).
    PartialTrackerModule m;
    m.inputs[2 * kN + 1].voltage = 0.f; // smooth = 0
    set_peak(m, 0, 0.3f, 0.8f);
    fire_trigger(m);
    tick(m, 4);

    set_peak(m, 0, 0.32f, 0.75f);
    fire_trigger(m);

    REQUIRE(m.outputs[0].voltage == Approx(0.32f).margin(1e-4f));
    REQUIRE(m.outputs[kM].voltage == Approx(0.75f).margin(1e-4f));
}

// ---------------------------------------------------------------------------
// Voice death and decay
// ---------------------------------------------------------------------------

TEST_CASE("PartialTrackerModule: voice dies when no matching peak at next trigger (smooth=0)",
          "[partial-tracker]") {
    // Birth a voice, then trigger with no peaks → voice goes inactive, amp decays.
    PartialTrackerModule m;
    m.inputs[2 * kN + 1].voltage = 0.f;
    set_peak(m, 0, 0.4f, 0.8f);
    fire_trigger(m);
    REQUIRE(m.outputs[kM].voltage > 0.5f);

    // Clear all peaks, fire trigger.
    set_peak(m, 0, 0.f, 0.f); // zero amp = no valid peak
    fire_trigger(m);
    tick(m, 1);

    // With smooth=0, beta=1: amp decays to 0 immediately on the death sample.
    REQUIRE(m.outputs[kM].voltage == Approx(0.f).margin(1e-5f));
}

TEST_CASE("PartialTrackerModule: voice decays smoothly with smooth > 0", "[partial-tracker]") {
    PartialTrackerModule m;
    m.inputs[2 * kN + 1].voltage = 0.5f; // moderate smooth
    set_peak(m, 0, 0.4f, 0.8f);
    fire_trigger(m);

    // Let voice amp settle.
    tick(m, 256);
    const float amp_before = m.outputs[kM].voltage;

    // Kill the voice.
    set_peak(m, 0, 0.f, 0.f);
    fire_trigger(m);

    // After several samples, amp should be below pre-death value.
    tick(m, 16);
    REQUIRE(m.outputs[kM].voltage < amp_before);
}

TEST_CASE("PartialTrackerModule: dead voice slot is reused for new peak", "[partial-tracker]") {
    // Birth a voice, kill it (smooth=0 → instant death), then birth a new one.
    PartialTrackerModule m;
    m.inputs[2 * kN + 1].voltage = 0.f;
    set_peak(m, 0, 0.4f, 0.8f);
    fire_trigger(m);
    tick(m, 2);

    // Kill it.
    set_peak(m, 0, 0.f, 0.f);
    fire_trigger(m);
    tick(m, 2);
    REQUIRE(m.outputs[kM].voltage < 0.001f); // slot is dead

    // New peak at a different frequency.
    set_peak(m, 0, 0.7f, 0.6f);
    fire_trigger(m);
    tick(m, 1);

    // Some voice should have amp > 0 now.
    float total = 0.f;
    for (std::size_t j = 0; j < kM; ++j)
        total += m.outputs[kM + j].voltage;
    REQUIRE(total > 0.f);
}

// ---------------------------------------------------------------------------
// Frequency smoothing
// ---------------------------------------------------------------------------

TEST_CASE("PartialTrackerModule: smooth=0.5 — freq glides toward target over several samples",
          "[partial-tracker]") {
    // Birth at 0.3; then update to 0.32 (within threshold).  With smooth=0.5,
    // freq on the update sample = 0.3 + 0.5*(0.32-0.3) = 0.31.
    PartialTrackerModule m;
    m.inputs[2 * kN + 1].voltage = 0.f; // smooth = 0 for birth
    set_peak(m, 0, 0.3f, 0.8f);
    fire_trigger(m);
    tick(m, 4);

    // Switch to smooth=0.5 before second trigger.
    m.inputs[2 * kN + 1].voltage = 0.5f;
    set_peak(m, 0, 0.32f, 0.8f);
    fire_trigger(m);

    // After the trigger sample: freq = 0.3 + 0.5*(0.32-0.3) = 0.31
    REQUIRE(m.outputs[0].voltage > 0.3f);
    REQUIRE(m.outputs[0].voltage < 0.32f);
}

// ---------------------------------------------------------------------------
// Out-of-threshold peak creates new voice
// ---------------------------------------------------------------------------

TEST_CASE("PartialTrackerModule: peak beyond threshold creates independent new voice",
          "[partial-tracker]") {
    // Birth a voice at 0.2; then trigger with a peak at 0.8 (distance 0.6 > threshold).
    // The original voice dies (no match); the new peak births a new voice.
    PartialTrackerModule m;
    m.inputs[2 * kN + 1].voltage = 0.f;
    set_peak(m, 0, 0.2f, 0.8f);
    fire_trigger(m);
    tick(m, 2);

    set_peak(m, 0, 0.8f, 0.7f);
    fire_trigger(m);
    tick(m, 2);

    // The new peak at 0.8 should appear in some voice slot.
    bool found_08 = false;
    for (std::size_t j = 0; j < kM; ++j) {
        if (m.outputs[kM + j].voltage > 0.5f && m.outputs[j].voltage > 0.7f)
            found_08 = true;
    }
    REQUIRE(found_08);
}

// ---------------------------------------------------------------------------
// prepare() reset
// ---------------------------------------------------------------------------

TEST_CASE("PartialTrackerModule: prepare() zeroes all outputs and clears voice state",
          "[partial-tracker]") {
    PartialTrackerModule m;
    m.inputs[2 * kN + 1].voltage = 0.f;
    set_peak(m, 0, 0.4f, 0.8f);
    fire_trigger(m);
    tick(m, 4);

    m.prepare(kArgs);

    for (const auto& out : m.outputs)
        REQUIRE(out.voltage == 0.f);

    // After prepare, a new trigger with no peaks → still zero.
    set_peak(m, 0, 0.f, 0.f);
    fire_trigger(m);
    tick(m, 4);
    for (const auto& out : m.outputs)
        REQUIRE(out.voltage == 0.f);
}

// ---------------------------------------------------------------------------
// Copy / move protection
// ---------------------------------------------------------------------------

static_assert(!std::is_copy_constructible_v<PartialTrackerModule>,
              "PartialTrackerModule must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<PartialTrackerModule>,
              "PartialTrackerModule must not be copy-assignable");
