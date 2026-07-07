// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the kairos/patch-bus custom CLAP extension.
//
// clap_plugin.cpp is compiled directly into the kairos-grid-plugin-tests binary
// (see tests/CMakeLists.txt) so the factory and extension can be exercised
// without dlopen().

#include <clap/clap.h>
#include <kairos_grid/clap_kairos_param_bus.h>
#include <kairos_grid/clap_kairos_patch_bus.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Stub host and factory helpers (same pattern as test_clap_plugin.cpp)
// ---------------------------------------------------------------------------

namespace {

clap_host_t make_stub_host() {
    clap_host_t h{};
    h.clap_version     = CLAP_VERSION_INIT;
    h.name             = "test-host";
    h.vendor           = "kairos-grid-tests";
    h.url              = "";
    h.version          = "0";
    h.get_extension    = [](const clap_host_t*, const char*) -> const void* { return nullptr; };
    h.request_restart  = [](const clap_host_t*) {};
    h.request_process  = [](const clap_host_t*) {};
    h.request_callback = [](const clap_host_t*) {};
    return h;
}

extern "C" const clap_plugin_entry_t clap_entry;

const clap_plugin_t* create_plugin(const clap_host_t* host) {
    clap_entry.init("");
    const auto* factory =
        static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory)
        return nullptr;
    return factory->create_plugin(factory, host, "studio.nomos.kairos-grid");
}

struct SilentProcess {
    static constexpr uint32_t k_block = 64;
    std::vector<float>        in_l, in_r, out_l, out_r;
    float*                    in_data[2];
    float*                    out_data[2];
    clap_audio_buffer_t       audio_in{};
    clap_audio_buffer_t       audio_out{};
    clap_input_events_t       in_evts{};
    clap_output_events_t      out_evts{};
    clap_process_t            proc{};

    SilentProcess()
        : in_l(k_block, 0.f), in_r(k_block, 0.f), out_l(k_block, 0.f), out_r(k_block, 0.f) {
        in_data[0]              = in_l.data();
        in_data[1]              = in_r.data();
        out_data[0]             = out_l.data();
        out_data[1]             = out_r.data();
        audio_in.data32         = in_data;
        audio_in.channel_count  = 2;
        audio_out.data32        = out_data;
        audio_out.channel_count = 2;
        in_evts.ctx             = nullptr;
        in_evts.size            = [](const clap_input_events_t*) -> uint32_t { return 0; };
        in_evts.get = [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* {
            return nullptr;
        };
        out_evts.ctx      = nullptr;
        out_evts.try_push = [](const clap_output_events_t*, const clap_event_header_t*) -> bool {
            return true;
        };
        proc.frames_count        = k_block;
        proc.transport           = nullptr;
        proc.audio_inputs        = &audio_in;
        proc.audio_inputs_count  = 1;
        proc.audio_outputs       = &audio_out;
        proc.audio_outputs_count = 1;
        proc.in_events           = &in_evts;
        proc.out_events          = &out_evts;
    }
};

// EDN descriptor for a minimal passthrough: env + audio-in + audio-out, stereo cables.
static constexpr const char* k_passthrough_edn =
    "{:modules [{:type \"env\"} {:type \"audio-in\"} {:type \"audio-out\"}]"
    " :cables [[1 0 2 0] [1 1 2 1]]}";

// Descriptor with env + audio-out only (no audio-in, no cables — silent by construction).
static constexpr const char* k_env_out_edn =
    "{:modules [{:type \"env\"} {:type \"audio-out\"}] :cables []}";

} // namespace

// ---------------------------------------------------------------------------
// Extension registration
// ---------------------------------------------------------------------------

TEST_CASE("patch-bus: extension is exposed after init") {
    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p != nullptr);
    REQUIRE(p->init(p));

    REQUIRE(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS) != nullptr);
    p->destroy(p);
}

TEST_CASE("patch-bus: push_patch returns false for null descriptor") {
    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
    REQUIRE(pb != nullptr);

    REQUIRE(pb->push_patch(p, nullptr, 0) == false);
    REQUIRE(pb->push_patch(p, "", 0) == false);
    p->destroy(p);
}

TEST_CASE("patch-bus: push_patch returns false for unknown module type") {
    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));

    const char* bad = "{:modules [{:type \"not-a-real-module\"}] :cables []}";
    REQUIRE(pb->push_patch(p, bad, static_cast<uint32_t>(std::strlen(bad))) == false);
    p->destroy(p);
}

TEST_CASE("patch-bus: get_patch returns null before any push") {
    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));

    REQUIRE(pb->get_patch(p) == nullptr);
    p->destroy(p);
}

TEST_CASE("patch-bus: push_patch returns true for valid descriptor") {
    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));

    const uint32_t len = static_cast<uint32_t>(std::strlen(k_passthrough_edn));
    REQUIRE(pb->push_patch(p, k_passthrough_edn, len) == true);
    p->destroy(p);
}

TEST_CASE("patch-bus: get_patch returns accepted descriptor immediately") {
    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));

    const uint32_t len = static_cast<uint32_t>(std::strlen(k_passthrough_edn));
    REQUIRE(pb->push_patch(p, k_passthrough_edn, len));

    const char* stored = pb->get_patch(p);
    REQUIRE(stored != nullptr);
    REQUIRE(std::string(stored) == std::string(k_passthrough_edn));
    p->destroy(p);
}

// ---------------------------------------------------------------------------
// Engine swap
// ---------------------------------------------------------------------------

TEST_CASE("patch-bus: param schema is updated after push_patch + process") {
    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
    const auto* param =
        static_cast<const clap_plugin_param_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const uint32_t epoch_before = param->get_schema(p)->epoch;

    // Push a different topology
    const uint32_t len = static_cast<uint32_t>(std::strlen(k_env_out_edn));
    REQUIRE(pb->push_patch(p, k_env_out_edn, len));

    // Swap happens at block start
    SilentProcess sp;
    REQUIRE(p->process(p, &sp.proc) != CLAP_PROCESS_ERROR);

    // env + audio-out topology still has 7 EnvironmentModule param ports
    const clap_kairos_param_schema_t* schema = param->get_schema(p);
    REQUIRE(schema->count == 7);
    REQUIRE(schema->epoch != epoch_before);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("patch-bus: param schema names are correct after push_patch + process") {
    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
    const auto* param =
        static_cast<const clap_plugin_param_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const uint32_t len = static_cast<uint32_t>(std::strlen(k_passthrough_edn));
    REQUIRE(pb->push_patch(p, k_passthrough_edn, len));

    SilentProcess sp;
    REQUIRE(p->process(p, &sp.proc) != CLAP_PROCESS_ERROR);

    const auto* schema = param->get_schema(p);
    REQUIRE(schema->count == 7);

    bool has_tempo = false, has_gate = false;
    for (uint32_t i = 0; i < schema->count; ++i) {
        if (std::strcmp(schema->entries[i].name, "env/tempo_hz") == 0)
            has_tempo = true;
        if (std::strcmp(schema->entries[i].name, "env/voice_gate") == 0)
            has_gate = true;
    }
    REQUIRE(has_tempo);
    REQUIRE(has_gate);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("patch-bus: push_patch supersedes an unprocessed pending slot") {
    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));

    // Push twice without processing — second supersedes first
    const uint32_t len1 = static_cast<uint32_t>(std::strlen(k_env_out_edn));
    REQUIRE(pb->push_patch(p, k_env_out_edn, len1));
    const uint32_t len2 = static_cast<uint32_t>(std::strlen(k_passthrough_edn));
    REQUIRE(pb->push_patch(p, k_passthrough_edn, len2));

    // get_patch immediately reflects the latest push
    const char* stored = pb->get_patch(p);
    REQUIRE(stored != nullptr);
    REQUIRE(std::string(stored) == std::string(k_passthrough_edn));

    SilentProcess sp;
    REQUIRE(p->process(p, &sp.proc) != CLAP_PROCESS_ERROR);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("patch-bus: process() is safe without any push_patch") {
    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    // Default engine (no push_patch) — should continue processing normally
    SilentProcess sp;
    for (int i = 0; i < 4; ++i)
        REQUIRE(p->process(p, &sp.proc) != CLAP_PROCESS_ERROR);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}
