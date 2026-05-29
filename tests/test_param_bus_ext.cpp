// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the kairos/param-bus custom CLAP extension.
//
// clap_plugin.cpp is compiled directly into the kairos-grid-plugin-tests binary
// (see tests/CMakeLists.txt) so the factory and extension can be exercised
// without dlopen().

#include <kairos_grid/clap_kairos_param_bus.h>
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

// Build a minimal silent process context.
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

TEST_CASE("param-bus: extension is exposed after init") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p != nullptr);
    REQUIRE(p->init(p));

    const void* ext = p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS);
    REQUIRE(ext != nullptr);

    p->destroy(p);
}

TEST_CASE("param-bus: unknown extension id returns null") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));

    REQUIRE(p->get_extension(p, "kairos/not-a-real-extension") == nullptr);

    p->destroy(p);
}

// ---------------------------------------------------------------------------
// Schema — default graph: EnvironmentModule has 7 param ports
// ---------------------------------------------------------------------------

TEST_CASE("param-bus: schema returns valid pointer after activate") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* pb = static_cast<const clap_plugin_param_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));
    REQUIRE(pb != nullptr);

    const clap_kairos_param_schema_t* schema = pb->get_schema(p);
    REQUIRE(schema != nullptr);

    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("param-bus: default graph schema has non-zero count and epoch") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* pb = static_cast<const clap_plugin_param_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const clap_kairos_param_schema_t* schema = pb->get_schema(p);
    // EnvironmentModule registers 7 param ports.
    REQUIRE(schema->count   == 7);
    REQUIRE(schema->entries != nullptr);
    REQUIRE(schema->epoch   >  0);

    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("param-bus: schema entry names match EnvironmentModule ports") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* pb = static_cast<const clap_plugin_param_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const clap_kairos_param_schema_t* schema = pb->get_schema(p);
    REQUIRE(schema->count >= 7);

    // Collect names into a set for order-independent check.
    bool has_tempo    = false;
    bool has_beat     = false;
    bool has_bar      = false;
    bool has_playing  = false;
    bool has_note     = false;
    bool has_gate     = false;
    bool has_velocity = false;
    for (uint32_t i = 0; i < schema->count; ++i) {
        const char* n = schema->entries[i].name;
        if (std::strcmp(n, "env/tempo_hz")        == 0) has_tempo    = true;
        if (std::strcmp(n, "env/beat_phase")       == 0) has_beat     = true;
        if (std::strcmp(n, "env/bar_phase")        == 0) has_bar      = true;
        if (std::strcmp(n, "env/is_playing")       == 0) has_playing  = true;
        if (std::strcmp(n, "env/voice_note")       == 0) has_note     = true;
        if (std::strcmp(n, "env/voice_gate")       == 0) has_gate     = true;
        if (std::strcmp(n, "env/voice_velocity")   == 0) has_velocity = true;
    }
    REQUIRE(has_tempo);
    REQUIRE(has_beat);
    REQUIRE(has_bar);
    REQUIRE(has_playing);
    REQUIRE(has_note);
    REQUIRE(has_gate);
    REQUIRE(has_velocity);

    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("param-bus: schema epoch is stable across process() calls") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb = static_cast<const clap_plugin_param_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const uint32_t epoch_before = pb->get_schema(p)->epoch;

    SilentProcess sp;
    for (int i = 0; i < 4; ++i)
        REQUIRE(p->process(p, &sp.proc) != CLAP_PROCESS_ERROR);

    REQUIRE(pb->get_schema(p)->epoch == epoch_before);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("param-bus: epoch increments on re-activate") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* pb = static_cast<const clap_plugin_param_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const uint32_t epoch1 = pb->get_schema(p)->epoch;

    p->deactivate(p);
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const uint32_t epoch2 = pb->get_schema(p)->epoch;
    REQUIRE(epoch2 > epoch1);

    p->deactivate(p);
    p->destroy(p);
}

// ---------------------------------------------------------------------------
// set_param_frame — write path
// ---------------------------------------------------------------------------

TEST_CASE("param-bus: set_param_frame returns true after init (engine built in init)") {
    // build_engine() is called from init(), so param_frame_ is populated before
    // activate() is called.  set_param_frame() is therefore valid immediately
    // after init() — callers need not wait for activate().
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));

    const auto* pb = static_cast<const clap_plugin_param_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const uint32_t count = pb->get_schema(p)->count;
    REQUIRE(count > 0);
    std::vector<float> frame(count, 0.1f);
    REQUIRE(pb->set_param_frame(p, frame.data(), count) == true);

    p->destroy(p);
}

TEST_CASE("param-bus: set_param_frame returns true after activate") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* pb = static_cast<const clap_plugin_param_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const uint32_t count = pb->get_schema(p)->count;
    std::vector<float> frame(count, 0.5f);
    REQUIRE(pb->set_param_frame(p, frame.data(), count) == true);

    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("param-bus: set_param_frame values are reflected in params_get_value") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* pb = static_cast<const clap_plugin_param_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));
    const auto* params = static_cast<const clap_plugin_params_t*>(
        p->get_extension(p, CLAP_EXT_PARAMS));
    REQUIRE(params != nullptr);

    const clap_kairos_param_schema_t* schema = pb->get_schema(p);
    REQUIRE(schema->count > 0);

    // Write a distinctive value into entry[0].
    std::vector<float> frame(schema->count, 0.f);
    frame[0] = 0.42f;
    REQUIRE(pb->set_param_frame(p, frame.data(), schema->count));

    // params_get_value reads from param_frame_[id].
    double read_back = -1.0;
    REQUIRE(params->get_value(p, static_cast<clap_id>(schema->entries[0].id), &read_back));
    REQUIRE(static_cast<float>(read_back) == 0.42f);

    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("param-bus: set_param_frame with count > schema ignores excess") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* pb = static_cast<const clap_plugin_param_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const uint32_t schema_count = pb->get_schema(p)->count;
    // Send more values than there are params — should not crash.
    std::vector<float> frame(schema_count + 16, 0.1f);
    REQUIRE(pb->set_param_frame(p, frame.data(),
                                static_cast<uint32_t>(frame.size())) == true);

    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("param-bus: reset bumps epoch and clears param frame") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* pb = static_cast<const clap_plugin_param_bus_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const uint32_t epoch_before = pb->get_schema(p)->epoch;
    p->reset(p);
    const uint32_t epoch_after  = pb->get_schema(p)->epoch;

    REQUIRE(epoch_after > epoch_before);
    REQUIRE(pb->get_schema(p)->count == 7); // EnvironmentModule still has 7 ports

    p->deactivate(p);
    p->destroy(p);
}
