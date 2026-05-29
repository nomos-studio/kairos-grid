// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the kairos/tap-bus custom CLAP extension.
//
// clap_plugin.cpp is compiled directly into the kairos-grid-plugin-tests binary
// (see tests/CMakeLists.txt) so the factory and extension can be exercised
// without dlopen().

#include <kairos_grid/clap_kairos_tap_bus.h>
#include <clap/clap.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Stub host and factory helpers (same pattern as test_clap_plugin.cpp)
// ---------------------------------------------------------------------------

namespace {

clap_host_t make_stub_host() {
    clap_host_t h{};
    h.clap_version      = CLAP_VERSION_INIT;
    h.name              = "test-host";
    h.vendor            = "kairos-grid-tests";
    h.url               = "";
    h.version           = "0";
    h.get_extension     = [](const clap_host_t*, const char*) -> const void* { return nullptr; };
    h.request_restart   = [](const clap_host_t*) {};
    h.request_process   = [](const clap_host_t*) {};
    h.request_callback  = [](const clap_host_t*) {};
    return h;
}

extern "C" const clap_plugin_entry_t clap_entry;

const clap_plugin_t* create_plugin(const clap_host_t* host) {
    clap_entry.init("");
    const auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) return nullptr;
    return factory->create_plugin(factory, host, "studio.nomos.kairos-grid");
}

// Build a minimal silent process context so we can call plugin->process().
struct SilentProcess {
    static constexpr uint32_t k_block = 64;

    std::vector<float> in_l, in_r, out_l, out_r;
    float* in_data[2];
    float* out_data[2];
    clap_audio_buffer_t audio_in{};
    clap_audio_buffer_t audio_out{};
    clap_input_events_t  in_evts{};
    clap_output_events_t out_evts{};
    clap_process_t proc{};

    SilentProcess() : in_l(k_block, 0.f), in_r(k_block, 0.f),
                      out_l(k_block, 0.f), out_r(k_block, 0.f) {
        in_data[0]  = in_l.data();  in_data[1]  = in_r.data();
        out_data[0] = out_l.data(); out_data[1] = out_r.data();

        audio_in.data32        = in_data;
        audio_in.channel_count = 2;
        audio_out.data32        = out_data;
        audio_out.channel_count = 2;

        in_evts.ctx  = nullptr;
        in_evts.size = [](const clap_input_events_t*) -> uint32_t { return 0; };
        in_evts.get  = [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* {
            return nullptr;
        };
        out_evts.ctx      = nullptr;
        out_evts.try_push = [](const clap_output_events_t*, const clap_event_header_t*) -> bool {
            return true;
        };

        proc.frames_count      = k_block;
        proc.transport         = nullptr;
        proc.audio_inputs       = &audio_in;
        proc.audio_inputs_count = 1;
        proc.audio_outputs       = &audio_out;
        proc.audio_outputs_count = 1;
        proc.in_events           = &in_evts;
        proc.out_events          = &out_evts;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Extension registration
// ---------------------------------------------------------------------------

TEST_CASE("tap-bus: extension is exposed after init") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p != nullptr);
    REQUIRE(p->init(p));

    const void* ext = p->get_extension(p, CLAP_EXT_KAIROS_TAP_BUS);
    REQUIRE(ext != nullptr);

    p->destroy(p);
}

TEST_CASE("tap-bus: unknown extension id returns null") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));

    REQUIRE(p->get_extension(p, "kairos/not-a-real-extension") == nullptr);

    p->destroy(p);
}

// ---------------------------------------------------------------------------
// Schema — default graph has 0 taps (EnvironmentModule + Audio I/O only)
// ---------------------------------------------------------------------------

TEST_CASE("tap-bus: schema returns valid pointer after activate") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* tb = static_cast<const clap_plugin_tap_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_TAP_BUS));
    REQUIRE(tb != nullptr);

    const clap_kairos_tap_schema_t* schema = tb->get_schema(p);
    REQUIRE(schema != nullptr);

    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("tap-bus: default graph schema has 0 taps and non-zero epoch") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* tb = static_cast<const clap_plugin_tap_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_TAP_BUS));

    const clap_kairos_tap_schema_t* schema = tb->get_schema(p);
    REQUIRE(schema->count   == 0);
    REQUIRE(schema->entries == nullptr);
    REQUIRE(schema->epoch   >  0);

    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("tap-bus: schema epoch is stable across process() calls") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* tb = static_cast<const clap_plugin_tap_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_TAP_BUS));

    const uint32_t epoch_before = tb->get_schema(p)->epoch;

    SilentProcess sp;
    for (int i = 0; i < 4; ++i)
        REQUIRE(p->process(p, &sp.proc) != CLAP_PROCESS_ERROR);

    REQUIRE(tb->get_schema(p)->epoch == epoch_before);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("tap-bus: epoch increments on re-activate") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* tb = static_cast<const clap_plugin_tap_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_TAP_BUS));

    const uint32_t epoch1 = tb->get_schema(p)->epoch;

    p->deactivate(p);
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const uint32_t epoch2 = tb->get_schema(p)->epoch;
    REQUIRE(epoch2 > epoch1);

    p->deactivate(p);
    p->destroy(p);
}

// ---------------------------------------------------------------------------
// tap_frame — default graph produces an empty frame
// ---------------------------------------------------------------------------

TEST_CASE("tap-bus: get_tap_frame after process() returns count 0 for default graph") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* tb = static_cast<const clap_plugin_tap_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_TAP_BUS));

    SilentProcess sp;
    REQUIRE(p->process(p, &sp.proc) != CLAP_PROCESS_ERROR);

    uint32_t count = 99;
    const float* frame = tb->get_tap_frame(p, &count);
    REQUIRE(count == 0);
    // frame may be null when count == 0; no dereference
    (void)frame;

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("tap-bus: get_tap_frame count matches schema count") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* tb = static_cast<const clap_plugin_tap_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_TAP_BUS));

    SilentProcess sp;
    REQUIRE(p->process(p, &sp.proc) != CLAP_PROCESS_ERROR);

    const uint32_t schema_count = tb->get_schema(p)->count;
    uint32_t frame_count = 0;
    tb->get_tap_frame(p, &frame_count);
    REQUIRE(frame_count == schema_count);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("tap-bus: reset bumps epoch and keeps tap count consistent") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* tb = static_cast<const clap_plugin_tap_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_TAP_BUS));

    const uint32_t epoch_before = tb->get_schema(p)->epoch;
    p->reset(p);
    const uint32_t epoch_after  = tb->get_schema(p)->epoch;

    REQUIRE(epoch_after > epoch_before);
    REQUIRE(tb->get_schema(p)->count == 0); // default graph still has no taps

    p->deactivate(p);
    p->destroy(p);
}
