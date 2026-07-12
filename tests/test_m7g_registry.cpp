// SPDX-License-Identifier: GPL-3.0-or-later
// Integration tests for the M7g module registry — verifies that all newly
// registered module types are reachable through the patch-bus EDN interface
// and that their param-bus schema entries match expectations.
//
// Build requirements by section:
//   MI tests    → KAIROS_GRID_PLUGIN_HAS_MI    (plugin-mi preset)
//   Surge tests → KAIROS_GRID_PLUGIN_HAS_SURGE (plugin-mi-surge preset, future)
//
// The `surge` preset disables KAIROS_GRID_BUILD_PLUGIN due to a CLAP target
// collision between clap-juce-extensions (bundled by Surge) and the plugin
// preset's FetchContent clap target.  Surge registry entries are compiled into
// clap_plugin.cpp when KAIROS_GRID_PLUGIN_HAS_SURGE is defined; the full
// end-to-end integration tests for those entries wait until the collision is
// resolved and a combined preset exists.

#include <clap/clap.h>
#include <kairos_grid/clap_kairos_param_bus.h>
#include <kairos_grid/clap_kairos_patch_bus.h>
#include <kairos_grid/clap_kairos_tap_bus.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// Stub host + factory helpers (same pattern as other CLAP extension tests)
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

// Push an EDN patch, tick one block, and return the set of param port names in
// the schema.  Returns an empty set if push_patch or process() fails.
std::unordered_set<std::string> push_and_get_schema(const clap_plugin_t*           p,
                                                    const clap_plugin_patch_bus_t* pb,
                                                    const clap_plugin_param_bus_t* param,
                                                    const char*                    edn) {
    const auto len = static_cast<uint32_t>(std::strlen(edn));
    if (!pb->push_patch(p, edn, len))
        return {};

    SilentProcess sp;
    if (p->process(p, &sp.proc) == CLAP_PROCESS_ERROR)
        return {};

    const auto* schema = param->get_schema(p);
    if (!schema)
        return {};

    std::unordered_set<std::string> names;
    for (uint32_t i = 0; i < schema->count; ++i)
        names.emplace(schema->entries[i].name);
    return names;
}

// Return the set of tap names from the tap schema.
std::unordered_set<std::string> get_tap_names(const clap_plugin_t*         p,
                                              const clap_plugin_tap_bus_t* tb) {
    const auto* schema = tb->get_schema(p);
    if (!schema)
        return {};
    std::unordered_set<std::string> names;
    for (uint32_t i = 0; i < schema->count; ++i)
        names.emplace(schema->entries[i].name);
    return names;
}

} // namespace

// ---------------------------------------------------------------------------
// MI — one-pole filter (available under plugin-mi preset)
// ---------------------------------------------------------------------------

#if defined(KAIROS_GRID_PLUGIN_HAS_MI)

TEST_CASE("m7g registry: lpg — push_patch succeeds and exposes cv and character params") {
    // env(0) + audio-in(1) + lpg(2) + audio-out(3)
    // Audio-in L/R → lpg in-L/in-R; lpg out-L/R → audio-out
    static constexpr const char* k_edn = "{:modules [{:type \"env\"} {:type \"audio-in\"}"
                                         "           {:type \"lpg\"} {:type \"audio-out\"}]"
                                         " :cables [[1 0 2 0] [1 1 2 1] [2 0 3 0] [2 1 3 1]]}";

    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
    const auto* param =
        static_cast<const clap_plugin_param_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const auto names = push_and_get_schema(p, pb, param, k_edn);
    REQUIRE_FALSE(names.empty());
    CHECK(names.count("lpg/cv") == 1);
    CHECK(names.count("lpg/character") == 1);
    // decay is not exposed for the clean variant
    CHECK(names.count("lpg/decay") == 0);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("m7g registry: lpg-vactrol — push_patch succeeds and exposes cv, decay, and character") {
    static constexpr const char* k_edn = "{:modules [{:type \"env\"} {:type \"audio-in\"}"
                                         "           {:type \"lpg-vactrol\"} {:type \"audio-out\"}]"
                                         " :cables [[1 0 2 0] [1 1 2 1] [2 0 3 0] [2 1 3 1]]}";

    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
    const auto* param =
        static_cast<const clap_plugin_param_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const auto names = push_and_get_schema(p, pb, param, k_edn);
    REQUIRE_FALSE(names.empty());
    CHECK(names.count("lpg-vactrol/cv") == 1);
    CHECK(names.count("lpg-vactrol/decay") == 1);
    CHECK(names.count("lpg-vactrol/character") == 1);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("m7g registry: ws-hard — push_patch succeeds and exposes drive param") {
    static constexpr const char* k_edn = "{:modules [{:type \"env\"} {:type \"audio-in\"}"
                                         "           {:type \"ws-hard\"} {:type \"audio-out\"}]"
                                         " :cables [[1 0 2 0] [1 1 2 1] [2 0 3 0] [2 1 3 1]]}";

    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
    const auto* param =
        static_cast<const clap_plugin_param_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const auto names = push_and_get_schema(p, pb, param, k_edn);
    REQUIRE_FALSE(names.empty());
    CHECK(names.count("ws-hard/drive") == 1);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("m7g registry: all waveshaper types — push_patch returns true") {
    static constexpr const char* k_types[] = {"ws-hard", "ws-tanh", "ws-soft", "ws-fold", "folder"};

    for (const auto* type : k_types) {
        const std::string edn = std::string("{:modules [{:type \"env\"} {:type \"audio-in\"}"
                                            "           {:type \"") +
                                type +
                                "\"} {:type \"audio-out\"}]"
                                " :cables [[1 0 2 0] [1 1 2 1] [2 0 3 0] [2 1 3 1]]}";

        auto        host = make_stub_host();
        const auto* p    = create_plugin(&host);
        REQUIRE(p->init(p));
        REQUIRE(p->activate(p, 48000.0, 1, 512));
        REQUIRE(p->start_processing(p));

        const auto* pb = static_cast<const clap_plugin_patch_bus_t*>(
            p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));

        const auto len = static_cast<uint32_t>(edn.size());
        INFO("waveshaper type: " << type);
        REQUIRE(pb->push_patch(p, edn.c_str(), len) == true);

        SilentProcess sp;
        REQUIRE(p->process(p, &sp.proc) != CLAP_PROCESS_ERROR);

        p->stop_processing(p);
        p->deactivate(p);
        p->destroy(p);
    }
}

TEST_CASE("m7g registry: one-pole — push_patch succeeds and exposes freq param") {
    // env(0) + audio-in(1) + one-pole(2) + audio-out(3)
    // Audio-in L → one-pole in; one-pole lp → out L; one-pole hp → out R
    static constexpr const char* k_edn = "{:modules [{:type \"env\"} {:type \"audio-in\"}"
                                         "           {:type \"one-pole\"} {:type \"audio-out\"}]"
                                         " :cables [[1 0 2 0] [2 0 3 0] [2 1 3 1]]}";

    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
    const auto* param =
        static_cast<const clap_plugin_param_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const auto names = push_and_get_schema(p, pb, param, k_edn);
    REQUIRE_FALSE(names.empty());
    CHECK(names.count("one-pole/freq") == 1);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

#endif // KAIROS_GRID_PLUGIN_HAS_MI

// ---------------------------------------------------------------------------
// Surge filters, effects, oscillators, and modulators.
// Available only under a future plugin-mi-surge preset (see file header).
// ---------------------------------------------------------------------------

#if defined(KAIROS_GRID_PLUGIN_HAS_SURGE)

TEST_CASE("m7g registry: ladder — push_patch succeeds and exposes cutoff+resonance") {
    static constexpr const char* k_edn = "{:modules [{:type \"env\"} {:type \"audio-in\"}"
                                         "           {:type \"ladder\"} {:type \"audio-out\"}]"
                                         " :cables [[1 0 2 0] [1 1 2 1] [2 0 3 0] [2 1 3 1]]}";

    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
    const auto* param =
        static_cast<const clap_plugin_param_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const auto names = push_and_get_schema(p, pb, param, k_edn);
    REQUIRE_FALSE(names.empty());
    CHECK(names.count("ladder/cutoff") == 1);
    CHECK(names.count("ladder/resonance") == 1);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("m7g registry: all Surge filter types — push_patch returns true") {
    static constexpr const char* k_types[] = {"ladder",  "diode", "k35-lp", "k35-hp",
                                              "obxd-4p", "lp12",  "lp24",   "hp12",
                                              "hp24",    "bp12",  "bp24"};

    for (const auto* type : k_types) {
        const std::string edn = std::string("{:modules [{:type \"env\"} {:type \"audio-in\"}"
                                            "           {:type \"") +
                                type +
                                "\"} {:type \"audio-out\"}]"
                                " :cables [[1 0 2 0] [1 1 2 1] [2 0 3 0] [2 1 3 1]]}";

        auto        host = make_stub_host();
        const auto* p    = create_plugin(&host);
        REQUIRE(p->init(p));
        REQUIRE(p->activate(p, 48000.0, 1, 512));
        REQUIRE(p->start_processing(p));

        const auto* pb = static_cast<const clap_plugin_patch_bus_t*>(
            p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));

        const auto len = static_cast<uint32_t>(edn.size());
        INFO("filter type: " << type);
        REQUIRE(pb->push_patch(p, edn.c_str(), len) == true);

        SilentProcess sp;
        REQUIRE(p->process(p, &sp.proc) != CLAP_PROCESS_ERROR);

        p->stop_processing(p);
        p->deactivate(p);
        p->destroy(p);
    }
}

TEST_CASE("m7g registry: reverb1 — push_patch succeeds and exposes p0..p11") {
    static constexpr const char* k_edn = "{:modules [{:type \"env\"} {:type \"audio-in\"}"
                                         "           {:type \"reverb1\"} {:type \"audio-out\"}]"
                                         " :cables [[1 0 2 0] [1 1 2 1] [2 0 3 0] [2 1 3 1]]}";

    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
    const auto* param =
        static_cast<const clap_plugin_param_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const auto names = push_and_get_schema(p, pb, param, k_edn);
    REQUIRE_FALSE(names.empty());
    CHECK(names.count("reverb1/p0") == 1);
    CHECK(names.count("reverb1/p5") == 1);
    CHECK(names.count("reverb1/p11") == 1);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("m7g registry: all Surge effect types — push_patch returns true") {
    static constexpr const char* k_types[] = {"reverb1", "reverb2",  "chorus",
                                              "delay",   "phaser",   "flanger",
                                              "bonsai",  "ensemble", "distortion"};

    for (const auto* type : k_types) {
        const std::string edn = std::string("{:modules [{:type \"env\"} {:type \"audio-in\"}"
                                            "           {:type \"") +
                                type +
                                "\"} {:type \"audio-out\"}]"
                                " :cables [[1 0 2 0] [1 1 2 1] [2 0 3 0] [2 1 3 1]]}";

        auto        host = make_stub_host();
        const auto* p    = create_plugin(&host);
        REQUIRE(p->init(p));
        REQUIRE(p->activate(p, 48000.0, 1, 512));
        REQUIRE(p->start_processing(p));

        const auto* pb = static_cast<const clap_plugin_patch_bus_t*>(
            p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));

        const auto len = static_cast<uint32_t>(edn.size());
        INFO("effect type: " << type);
        REQUIRE(pb->push_patch(p, edn.c_str(), len) == true);

        SilentProcess sp;
        REQUIRE(p->process(p, &sp.proc) != CLAP_PROCESS_ERROR);

        p->stop_processing(p);
        p->deactivate(p);
        p->destroy(p);
    }
}

TEST_CASE("m7g registry: classic osc — push_patch succeeds and exposes p0..p6") {
    // env voice_note (output port 4) → classic note (input port 0)
    static constexpr const char* k_edn =
        "{:modules [{:type \"env\"} {:type \"classic\"} {:type \"audio-out\"}]"
        " :cables [[0 4 1 0] [1 0 2 0] [1 1 2 1]]}";

    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
    const auto* param =
        static_cast<const clap_plugin_param_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const auto names = push_and_get_schema(p, pb, param, k_edn);
    REQUIRE_FALSE(names.empty());
    CHECK(names.count("classic/p0") == 1);
    CHECK(names.count("classic/p3") == 1);
    CHECK(names.count("classic/p6") == 1);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("m7g registry: all Surge oscillator types — push_patch returns true") {
    static constexpr const char* k_types[] = {"classic", "sine-osc", "modern", "fm2",
                                              "fm3",     "string",   "twist"};

    for (const auto* type : k_types) {
        const std::string edn = std::string("{:modules [{:type \"env\"} {:type \"") + type +
                                "\"} {:type \"audio-out\"}]"
                                " :cables [[0 4 1 0] [1 0 2 0] [1 1 2 1]]}";

        auto        host = make_stub_host();
        const auto* p    = create_plugin(&host);
        REQUIRE(p->init(p));
        REQUIRE(p->activate(p, 48000.0, 1, 512));
        REQUIRE(p->start_processing(p));

        const auto* pb = static_cast<const clap_plugin_patch_bus_t*>(
            p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));

        const auto len = static_cast<uint32_t>(edn.size());
        INFO("osc type: " << type);
        REQUIRE(pb->push_patch(p, edn.c_str(), len) == true);

        SilentProcess sp;
        REQUIRE(p->process(p, &sp.proc) != CLAP_PROCESS_ERROR);

        p->stop_processing(p);
        p->deactivate(p);
        p->destroy(p);
    }
}

TEST_CASE("m7g registry: adsr — exposes attack/decay/sustain/release params and envelope tap") {
    // env voice_gate (output port 5) → adsr gate (input port 0)
    static constexpr const char* k_edn =
        "{:modules [{:type \"env\"} {:type \"adsr\"} {:type \"audio-out\"}]"
        " :cables [[0 5 1 0]]}";

    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
    const auto* param =
        static_cast<const clap_plugin_param_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));
    const auto* tb =
        static_cast<const clap_plugin_tap_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_TAP_BUS));

    const auto names = push_and_get_schema(p, pb, param, k_edn);
    REQUIRE_FALSE(names.empty());
    CHECK(names.count("adsr/attack") == 1);
    CHECK(names.count("adsr/decay") == 1);
    CHECK(names.count("adsr/sustain") == 1);
    CHECK(names.count("adsr/release") == 1);

    const auto taps = get_tap_names(p, tb);
    CHECK(taps.count("signal/envelope") == 1);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("m7g registry: lfo — exposes rate/deform/shape params and lfo tap") {
    static constexpr const char* k_edn =
        "{:modules [{:type \"env\"} {:type \"lfo\"} {:type \"audio-out\"}]"
        " :cables []}";

    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
    const auto* param =
        static_cast<const clap_plugin_param_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));
    const auto* tb =
        static_cast<const clap_plugin_tap_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_TAP_BUS));

    const auto names = push_and_get_schema(p, pb, param, k_edn);
    REQUIRE_FALSE(names.empty());
    CHECK(names.count("lfo/rate") == 1);
    CHECK(names.count("lfo/deform") == 1);
    CHECK(names.count("lfo/shape") == 1);

    const auto taps = get_tap_names(p, tb);
    CHECK(taps.count("signal/lfo") == 1);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

// Showcase: plaits (MI) → ladder (Surge) → reverb2 (Surge)
// Both MI and Surge must be available.
#if defined(KAIROS_GRID_PLUGIN_HAS_MI)

TEST_CASE("m7g registry: plaits -> ladder -> reverb2 chain builds and processes") {
    // env(0) plaits(1) ladder(2) reverb2(3) audio-out(4)
    // env note(4)→plaits note(0), env gate(5)→plaits trigger(4)
    // plaits L(0)→ladder in-L(0), plaits L(0)→ladder in-R(1)
    // ladder L(0)→reverb2 in-L(0), ladder R(1)→reverb2 in-R(1)
    // reverb2 L(0)→audio-out L(0), reverb2 R(1)→audio-out R(1)
    static constexpr const char* k_edn = "{:modules [{:type \"env\"} {:type \"plaits\"}"
                                         "           {:type \"ladder\"} {:type \"reverb2\"}"
                                         "           {:type \"audio-out\"}]"
                                         " :cables [[0 4 1 0] [0 5 1 4]"
                                         "          [1 0 2 0] [1 0 2 1]"
                                         "          [2 0 3 0] [2 1 3 1]"
                                         "          [3 0 4 0] [3 1 4 1]]}";

    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
    const auto* param =
        static_cast<const clap_plugin_param_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const auto names = push_and_get_schema(p, pb, param, k_edn);
    REQUIRE_FALSE(names.empty());
    CHECK(names.count("plaits/harmonics") == 1);
    CHECK(names.count("plaits/timbre") == 1);
    CHECK(names.count("ladder/cutoff") == 1);
    CHECK(names.count("ladder/resonance") == 1);
    CHECK(names.count("reverb2/p0") == 1);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

#endif // KAIROS_GRID_PLUGIN_HAS_MI (showcase)

// ---------------------------------------------------------------------------
// sst-waveshaper registry tests
// ---------------------------------------------------------------------------

TEST_CASE("m7g registry: sst-soft — push_patch succeeds and exposes drive param") {
    static constexpr const char* k_edn = "{:modules [{:type \"env\"} {:type \"audio-in\"}"
                                         "           {:type \"sst-soft\"} {:type \"audio-out\"}]"
                                         " :cables [[1 0 2 0] [1 1 2 1] [2 0 3 0] [2 1 3 1]]}";

    auto        host = make_stub_host();
    const auto* p    = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));
    REQUIRE(p->start_processing(p));

    const auto* pb =
        static_cast<const clap_plugin_patch_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
    const auto* param =
        static_cast<const clap_plugin_param_bus_t*>(p->get_extension(p, CLAP_EXT_KAIROS_PARAM_BUS));

    const auto names = push_and_get_schema(p, pb, param, k_edn);
    REQUIRE_FALSE(names.empty());
    CHECK(names.count("sst-soft/drive") == 1);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("m7g registry: all sst-waveshaper types — push_patch returns true") {
    static constexpr const char* k_types[] = {
        "sst-soft",     "sst-hard",     "sst-asym",       "sst-medium",
        "sst-ojd",      "sst-fuzz",     "sst-fuzz-heavy", "sst-westfold",
        "sst-dualfold", "sst-softfold", "sst-harmonic2",  "sst-harmonic3",
    };

    for (const auto* type : k_types) {
        const std::string edn = std::string("{:modules [{:type \"env\"} {:type \"audio-in\"}"
                                            "           {:type \"") +
                                type +
                                "\"} {:type \"audio-out\"}]"
                                " :cables [[1 0 2 0] [1 1 2 1] [2 0 3 0] [2 1 3 1]]}";

        auto        host = make_stub_host();
        const auto* p    = create_plugin(&host);
        REQUIRE(p->init(p));
        REQUIRE(p->activate(p, 48000.0, 1, 512));
        REQUIRE(p->start_processing(p));

        const auto* pb = static_cast<const clap_plugin_patch_bus_t*>(
            p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));

        const auto len = static_cast<uint32_t>(edn.size());
        INFO("sst-waveshaper type: " << type);
        REQUIRE(pb->push_patch(p, edn.c_str(), len) == true);

        SilentProcess sp;
        REQUIRE(p->process(p, &sp.proc) != CLAP_PROCESS_ERROR);

        p->stop_processing(p);
        p->deactivate(p);
        p->destroy(p);
    }
}

#endif // KAIROS_GRID_PLUGIN_HAS_SURGE
