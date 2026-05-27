// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for NLC-derived GridModules.

#include <kairos_grid/nlc/cv_channel_decoder_module.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace kairos_grid;
using namespace kairos_grid::nlc;

namespace {

static GridProcessArgs args_at_48k(int frame = 0)
{
    return {48000.f, 1.f / 48000.f, frame};
}

static void step(GridModule& m, int n = 1)
{
    auto a = args_at_48k();
    for (int i = 0; i < n; ++i) { a.frame = i; m.process(a); }
}

} // namespace

// ===========================================================================
// CvChannelDecoderModule — structure
// ===========================================================================

TEST_CASE("CvChannelDecoder: correct port counts") {
    CvChannelDecoderModule m;
    REQUIRE(m.inputs.size()  == 4);
    REQUIRE(m.outputs.size() == 9);
}

TEST_CASE("CvChannelDecoder: one velocity tap and two param ports") {
    CvChannelDecoderModule m;
    REQUIRE(m.taps.size()        == 1);
    REQUIRE(m.taps[0].name       == "signal/velocity");
    REQUIRE(m.param_ports.size() == 2);
    REQUIRE(m.param_ports[0].name == "cvdec/channels");
    REQUIRE(m.param_ports[1].name == "cvdec/space");
}

TEST_CASE("CvChannelDecoder: Output enum values are correct") {
    REQUIRE(CvChannelDecoderModule::k_ch0      == 0);
    REQUIRE(CvChannelDecoderModule::k_ch7      == 7);
    REQUIRE(CvChannelDecoderModule::k_velocity == 8);
}

// ===========================================================================
// CvChannelDecoderModule — N=1 Schmitt-trigger (degenerate case)
// ===========================================================================

TEST_CASE("CvChannelDecoder: N=1 gate fires when span is within band") {
    CvChannelDecoderModule m;
    m.inputs[CvChannelDecoderModule::k_channels].voltage = 1.f;
    m.inputs[CvChannelDecoderModule::k_space].voltage    = 0.4f;
    // center=0.5, hw = 0.4*0.5/1 = 0.2 → fires for span ∈ [0.3, 0.7]

    m.inputs[CvChannelDecoderModule::k_span].voltage = 0.5f;
    step(m);
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch0].voltage == 1.f);
}

TEST_CASE("CvChannelDecoder: N=1 gate is zero outside band") {
    CvChannelDecoderModule m;
    m.inputs[CvChannelDecoderModule::k_channels].voltage = 1.f;
    m.inputs[CvChannelDecoderModule::k_space].voltage    = 0.4f;
    // hw = 0.2; gate OFF for span < 0.3 or span > 0.7

    m.inputs[CvChannelDecoderModule::k_span].voltage = 0.1f;
    step(m);
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch0].voltage == 0.f);

    m.inputs[CvChannelDecoderModule::k_span].voltage = 0.9f;
    step(m);
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch0].voltage == 0.f);
}

// ===========================================================================
// CvChannelDecoderModule — multi-channel band detection
// ===========================================================================

TEST_CASE("CvChannelDecoder: N=4 activates correct band") {
    CvChannelDecoderModule m;
    m.inputs[CvChannelDecoderModule::k_channels].voltage = 4.f;
    m.inputs[CvChannelDecoderModule::k_space].voltage    = 0.8f;
    // N=4: centers at 0.125, 0.375, 0.625, 0.875; hw = 0.8*0.5/4 = 0.1

    const struct { float span; int active_ch; } cases[] = {
        {0.12f, 0},   // near center of ch0 (0.125)
        {0.38f, 1},   // near center of ch1 (0.375)
        {0.63f, 2},   // near center of ch2 (0.625)
        {0.88f, 3},   // near center of ch3 (0.875)
    };

    for (auto& c : cases) {
        m.inputs[CvChannelDecoderModule::k_span].voltage = c.span;
        step(m);
        INFO("span=" << c.span << " expected ch" << c.active_ch);
        REQUIRE(m.outputs[c.active_ch].voltage == 1.f);
        for (int j = 0; j < 4; ++j) {
            if (j != c.active_ch)
                REQUIRE(m.outputs[j].voltage == 0.f);
        }
    }
}

TEST_CASE("CvChannelDecoder: N=8 covers all bands with space=1") {
    CvChannelDecoderModule m;
    m.inputs[CvChannelDecoderModule::k_channels].voltage = 8.f;
    m.inputs[CvChannelDecoderModule::k_space].voltage    = 1.0f;
    // N=8: centers at i/8+0.0625 for i=0..7; hw = 1.0*0.5/8 = 0.0625

    for (int i = 0; i < 8; ++i) {
        const float center = (static_cast<float>(i) + 0.5f) / 8.f;
        m.inputs[CvChannelDecoderModule::k_span].voltage = center;
        step(m);
        INFO("ch" << i << " center=" << center);
        REQUIRE(m.outputs[i].voltage == 1.f);
    }
}

TEST_CASE("CvChannelDecoder: channels beyond N are always zero") {
    CvChannelDecoderModule m;
    m.inputs[CvChannelDecoderModule::k_channels].voltage = 3.f;
    m.inputs[CvChannelDecoderModule::k_space].voltage    = 1.0f;
    m.inputs[CvChannelDecoderModule::k_span].voltage     = 0.5f;
    step(m);

    // ch3..ch7 should be 0 regardless of span
    for (int i = 3; i < 8; ++i) {
        INFO("ch" << i << " should be 0");
        REQUIRE(m.outputs[i].voltage == 0.f);
    }
}

TEST_CASE("CvChannelDecoder: narrow space gives near-zero activation zone") {
    CvChannelDecoderModule m;
    m.inputs[CvChannelDecoderModule::k_channels].voltage = 4.f;
    m.inputs[CvChannelDecoderModule::k_space].voltage    = 0.0f;
    // hw=0: gate fires only when span == center exactly

    // Off-center: no gate fires
    m.inputs[CvChannelDecoderModule::k_span].voltage = 0.13f;
    step(m);
    for (int i = 0; i < 4; ++i)
        REQUIRE(m.outputs[i].voltage == 0.f);
}

// ===========================================================================
// CvChannelDecoderModule — velocity output
// ===========================================================================

TEST_CASE("CvChannelDecoder: velocity is zero for a constant signal") {
    CvChannelDecoderModule m;
    m.prepare(args_at_48k());
    m.inputs[CvChannelDecoderModule::k_span].voltage = 0.5f;
    step(m, 4);  // after the first sample last_span_ == 0.5, delta == 0
    REQUIRE_THAT(m.outputs[CvChannelDecoderModule::k_velocity].voltage,
                 Catch::Matchers::WithinAbs(0.f, 1e-6f));
    REQUIRE_THAT(m.taps[0].value,
                 Catch::Matchers::WithinAbs(0.f, 1e-6f));
}

TEST_CASE("CvChannelDecoder: velocity is positive after a step change") {
    CvChannelDecoderModule m;
    m.prepare(args_at_48k());

    m.inputs[CvChannelDecoderModule::k_span].voltage = 0.0f;
    m.process(args_at_48k(0));
    m.inputs[CvChannelDecoderModule::k_span].voltage = 0.5f;
    m.process(args_at_48k(1));  // delta = 0.5 → vel = 0.5 * 48000 >> 1 → clamped to 1

    REQUIRE(m.outputs[CvChannelDecoderModule::k_velocity].voltage > 0.f);
}

TEST_CASE("CvChannelDecoder: velocity never exceeds 1") {
    CvChannelDecoderModule m;
    m.prepare(args_at_48k());

    m.inputs[CvChannelDecoderModule::k_span].voltage = 0.0f;
    m.process(args_at_48k(0));
    m.inputs[CvChannelDecoderModule::k_span].voltage = 1.0f;  // maximum step
    m.process(args_at_48k(1));

    REQUIRE(m.outputs[CvChannelDecoderModule::k_velocity].voltage <= 1.f);
}

// ===========================================================================
// CvChannelDecoderModule — clock-gated mode
// ===========================================================================

TEST_CASE("CvChannelDecoder: free-run with no clock updates every sample") {
    CvChannelDecoderModule m;
    m.inputs[CvChannelDecoderModule::k_channels].voltage = 1.f;
    m.inputs[CvChannelDecoderModule::k_space].voltage    = 0.4f;
    m.inputs[CvChannelDecoderModule::k_clock].voltage    = 0.f;  // no clock

    // Sample 0: span=0.5 → gate fires
    m.inputs[CvChannelDecoderModule::k_span].voltage = 0.5f;
    m.process(args_at_48k(0));
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch0].voltage == 1.f);

    // Sample 1: span=0.1 → gate should update immediately (free-run)
    m.inputs[CvChannelDecoderModule::k_span].voltage = 0.1f;
    m.process(args_at_48k(1));
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch0].voltage == 0.f);
}

TEST_CASE("CvChannelDecoder: rising clock edge latches gate state") {
    CvChannelDecoderModule m;
    m.inputs[CvChannelDecoderModule::k_channels].voltage = 1.f;
    m.inputs[CvChannelDecoderModule::k_space].voltage    = 0.4f;
    // hw = 0.2; gate fires for span ∈ [0.3, 0.7]

    // Sample 0: clock HIGH → clocked mode begins; span=0.5 → gate=1
    m.inputs[CvChannelDecoderModule::k_clock].voltage = 1.f;
    m.inputs[CvChannelDecoderModule::k_span].voltage  = 0.5f;
    m.process(args_at_48k(0));
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch0].voltage == 1.f);

    // Sample 1: clock stays HIGH (not a new rising edge), span moves out
    m.inputs[CvChannelDecoderModule::k_span].voltage = 0.1f;
    m.process(args_at_48k(1));
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch0].voltage == 1.f);  // held

    // Sample 2: clock goes LOW — gate still held (no new edge)
    m.inputs[CvChannelDecoderModule::k_clock].voltage = 0.f;
    m.process(args_at_48k(2));
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch0].voltage == 1.f);  // held

    // Sample 3: clock goes HIGH again → new edge → gate updates to span=0.1 → 0
    m.inputs[CvChannelDecoderModule::k_clock].voltage = 1.f;
    m.process(args_at_48k(3));
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch0].voltage == 0.f);  // updated
}

TEST_CASE("CvChannelDecoder: prepare resets clocked mode to free-run") {
    CvChannelDecoderModule m;
    m.inputs[CvChannelDecoderModule::k_channels].voltage = 1.f;
    m.inputs[CvChannelDecoderModule::k_space].voltage    = 0.4f;

    // Trigger a clock to enter clocked mode
    m.inputs[CvChannelDecoderModule::k_clock].voltage = 1.f;
    m.inputs[CvChannelDecoderModule::k_span].voltage  = 0.5f;
    m.process(args_at_48k(0));
    // Clock goes LOW — in clocked mode, gate should hold
    m.inputs[CvChannelDecoderModule::k_clock].voltage = 0.f;
    m.inputs[CvChannelDecoderModule::k_span].voltage  = 0.1f;
    m.process(args_at_48k(1));
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch0].voltage == 1.f);  // held — confirms clocked mode

    // After prepare(), clocked mode is reset
    m.prepare(args_at_48k());
    m.inputs[CvChannelDecoderModule::k_clock].voltage = 0.f;
    m.inputs[CvChannelDecoderModule::k_span].voltage  = 0.1f;
    m.process(args_at_48k(0));
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch0].voltage == 0.f);  // free-run: updated
}

// ===========================================================================
// CvChannelDecoderModule — channels param updates at sample rate
// ===========================================================================

TEST_CASE("CvChannelDecoder: channels param can be changed mid-stream") {
    CvChannelDecoderModule m;
    m.inputs[CvChannelDecoderModule::k_space].voltage = 1.0f;
    m.inputs[CvChannelDecoderModule::k_span].voltage  = 0.9f;

    // With N=4: band centers at 0.125, 0.375, 0.625, 0.875; span=0.9 → near ch3
    m.inputs[CvChannelDecoderModule::k_channels].voltage = 4.f;
    m.process(args_at_48k(0));
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch3].voltage == 1.f);

    // Switch to N=2: band centers at 0.25, 0.75; span=0.9 → near ch1
    m.inputs[CvChannelDecoderModule::k_channels].voltage = 2.f;
    m.process(args_at_48k(1));
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch1].voltage == 1.f);
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch2].voltage == 0.f);
    REQUIRE(m.outputs[CvChannelDecoderModule::k_ch3].voltage == 0.f);
}
