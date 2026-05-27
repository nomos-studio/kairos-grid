// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the CLAP plugin wrapper — state save/load round-trip.
//
// clap_plugin.cpp is compiled directly into this test binary so the
// factory and entry point can be called without dlopen().
// A minimal stub clap_host_t satisfies the plugin's host pointer requirement.

#include <clap/clap.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Stub host — no extensions queried; callbacks never called in these tests.
// ---------------------------------------------------------------------------

namespace {

clap_host_t make_stub_host() {
    clap_host_t h{};
    h.clap_version  = CLAP_VERSION_INIT;
    h.host_data     = nullptr;
    h.name          = "test-host";
    h.vendor        = "kairos-grid-tests";
    h.url           = "";
    h.version       = "0";
    h.get_extension = [](const clap_host_t*, const char*) -> const void* { return nullptr; };
    h.request_restart   = [](const clap_host_t*) {};
    h.request_process   = [](const clap_host_t*) {};
    h.request_callback  = [](const clap_host_t*) {};
    return h;
}

// ---------------------------------------------------------------------------
// Memory-backed CLAP streams.
// ---------------------------------------------------------------------------

struct MemOStream {
    std::vector<uint8_t> buf;

    clap_ostream_t clap_stream() {
        clap_ostream_t s{};
        s.ctx = this;
        s.write = [](const clap_ostream_t* s, const void* data, uint64_t n) -> int64_t {
            auto* self = static_cast<MemOStream*>(s->ctx);
            const auto* p = static_cast<const uint8_t*>(data);
            self->buf.insert(self->buf.end(), p, p + n);
            return static_cast<int64_t>(n);
        };
        return s;
    }
};

struct MemIStream {
    const std::vector<uint8_t>& buf;
    std::size_t pos{0};

    explicit MemIStream(const std::vector<uint8_t>& b) : buf(b) {}

    clap_istream_t clap_stream() {
        clap_istream_t s{};
        s.ctx = this;
        s.read = [](const clap_istream_t* s, void* data, uint64_t n) -> int64_t {
            auto* self = static_cast<MemIStream*>(s->ctx);
            const std::size_t avail = self->buf.size() - self->pos;
            if (avail == 0) return 0;
            const std::size_t take = static_cast<std::size_t>(n) < avail
                                         ? static_cast<std::size_t>(n)
                                         : avail;
            std::memcpy(data, self->buf.data() + self->pos, take);
            self->pos += take;
            return static_cast<int64_t>(take);
        };
        return s;
    }
};

// ---------------------------------------------------------------------------
// Helpers — create a plugin instance via the factory entry point.
// clap_entry is defined in clap_plugin.cpp which is compiled into this binary.
// ---------------------------------------------------------------------------

extern "C" const clap_plugin_entry_t clap_entry;

const clap_plugin_t* create_plugin(const clap_host_t* host) {
    clap_entry.init("");
    const auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) return nullptr;
    return factory->create_plugin(factory, host, "studio.nomos.kairos-grid");
}

} // namespace

// ---------------------------------------------------------------------------
// Factory tests
// ---------------------------------------------------------------------------

TEST_CASE("clap_entry: factory returns one descriptor") {
    clap_entry.init("");
    const auto* f = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(f != nullptr);
    REQUIRE(f->get_plugin_count(f) == 1);
    const auto* d = f->get_plugin_descriptor(f, 0);
    REQUIRE(d != nullptr);
    REQUIRE(std::strcmp(d->id, "studio.nomos.kairos-grid") == 0);
}

TEST_CASE("clap_entry: unknown factory id returns null") {
    clap_entry.init("");
    REQUIRE(clap_entry.get_factory("unknown.factory") == nullptr);
}

TEST_CASE("clap_entry: unknown plugin id returns null") {
    clap_entry.init("");
    const auto* f = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(f != nullptr);
    auto host = make_stub_host();
    REQUIRE(f->create_plugin(f, &host, "unknown.plugin") == nullptr);
}

// ---------------------------------------------------------------------------
// Lifecycle tests
// ---------------------------------------------------------------------------

TEST_CASE("plugin: init + destroy succeeds") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p != nullptr);
    REQUIRE(p->init(p));
    p->destroy(p);
}

TEST_CASE("plugin: activate + deactivate succeeds") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 4096));
    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("plugin: exposes clap.state extension after init") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->get_extension(p, CLAP_EXT_STATE) != nullptr);
    p->destroy(p);
}

TEST_CASE("plugin: exposes clap.params and clap.audio-ports after init") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->get_extension(p, CLAP_EXT_PARAMS)      != nullptr);
    REQUIRE(p->get_extension(p, CLAP_EXT_AUDIO_PORTS) != nullptr);
    p->destroy(p);
}

// ---------------------------------------------------------------------------
// State save / load round-trip tests
// ---------------------------------------------------------------------------

TEST_CASE("state: save succeeds and produces non-empty blob") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* state = static_cast<const clap_plugin_state_t*>(
        p->get_extension(p, CLAP_EXT_STATE));
    REQUIRE(state != nullptr);

    MemOStream os;
    auto cs = os.clap_stream();
    REQUIRE(state->save(p, &cs));
    REQUIRE(os.buf.size() > 0);

    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("state: save + load round-trip preserves param values") {
    auto host = make_stub_host();

    // --- Save phase ---
    const auto* p1 = create_plugin(&host);
    REQUIRE(p1->init(p1));
    REQUIRE(p1->activate(p1, 48000.0, 1, 512));

    const auto* params = static_cast<const clap_plugin_params_t*>(
        p1->get_extension(p1, CLAP_EXT_PARAMS));
    const auto* state  = static_cast<const clap_plugin_state_t*>(
        p1->get_extension(p1, CLAP_EXT_STATE));
    REQUIRE(params != nullptr);
    REQUIRE(state  != nullptr);

    // Write non-default values via the params extension for all 7 env params.
    const uint32_t n = params->count(p1);
    REQUIRE(n == 7);

    // Use get_info to find the tempo and beat_phase param ids, then set them.
    clap_id tempo_id{CLAP_INVALID_ID}, beat_id{CLAP_INVALID_ID};
    for (uint32_t i = 0; i < n; ++i) {
        clap_param_info_t info{};
        REQUIRE(params->get_info(p1, i, &info));
        // leaf name is the part after the last '/'.
        if (std::strcmp(info.name, "tempo_hz")   == 0) tempo_id = info.id;
        if (std::strcmp(info.name, "beat_phase") == 0) beat_id  = info.id;
    }
    REQUIRE(tempo_id != CLAP_INVALID_ID);
    REQUIRE(beat_id  != CLAP_INVALID_ID);

    // Inject param values via a flush() call with a synthetic CLAP_EVENT_PARAM_VALUE.
    // Build the two events manually.
    auto make_param_event = [](clap_id pid, double val) {
        clap_event_param_value_t ev{};
        ev.header.size     = sizeof(ev);
        ev.header.time     = 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type     = CLAP_EVENT_PARAM_VALUE;
        ev.header.flags    = 0;
        ev.param_id        = pid;
        ev.cookie          = nullptr;
        ev.note_id         = -1;
        ev.port_index      = -1;
        ev.channel         = -1;
        ev.key             = -1;
        ev.value           = val;
        return ev;
    };

    auto ev_tempo = make_param_event(tempo_id, 0.75);
    auto ev_beat  = make_param_event(beat_id,  0.33);

    std::vector<clap_event_param_value_t> evs = {ev_tempo, ev_beat};

    clap_input_events_t in_evts{};
    in_evts.ctx = &evs;
    in_evts.size = [](const clap_input_events_t* q) -> uint32_t {
        return static_cast<uint32_t>(
            static_cast<const std::vector<clap_event_param_value_t>*>(q->ctx)->size());
    };
    in_evts.get = [](const clap_input_events_t* q, uint32_t i) -> const clap_event_header_t* {
        const auto& v = *static_cast<const std::vector<clap_event_param_value_t>*>(q->ctx);
        return &v[i].header;
    };

    clap_output_events_t out_evts{};
    out_evts.ctx      = nullptr;
    out_evts.try_push = [](const clap_output_events_t*, const clap_event_header_t*) -> bool {
        return true;
    };

    params->flush(p1, &in_evts, &out_evts);

    // Verify get_value reflects the new values.
    double v_tempo{}, v_beat{};
    REQUIRE(params->get_value(p1, tempo_id, &v_tempo));
    REQUIRE(params->get_value(p1, beat_id,  &v_beat));
    REQUIRE_THAT(v_tempo, Catch::Matchers::WithinAbs(0.75, 1e-6));
    REQUIRE_THAT(v_beat,  Catch::Matchers::WithinAbs(0.33, 1e-6));

    MemOStream os;
    auto cs = os.clap_stream();
    REQUIRE(state->save(p1, &cs));

    p1->deactivate(p1);
    p1->destroy(p1);

    // --- Load phase ---
    const auto* p2 = create_plugin(&host);
    REQUIRE(p2->init(p2));
    REQUIRE(p2->activate(p2, 48000.0, 1, 512));

    const auto* state2  = static_cast<const clap_plugin_state_t*>(
        p2->get_extension(p2, CLAP_EXT_STATE));
    const auto* params2 = static_cast<const clap_plugin_params_t*>(
        p2->get_extension(p2, CLAP_EXT_PARAMS));
    REQUIRE(state2  != nullptr);
    REQUIRE(params2 != nullptr);

    MemIStream is(os.buf);
    auto cis = is.clap_stream();
    REQUIRE(state2->load(p2, &cis));

    double v2_tempo{}, v2_beat{};
    REQUIRE(params2->get_value(p2, tempo_id, &v2_tempo));
    REQUIRE(params2->get_value(p2, beat_id,  &v2_beat));
    REQUIRE_THAT(v2_tempo, Catch::Matchers::WithinAbs(0.75, 1e-6));
    REQUIRE_THAT(v2_beat,  Catch::Matchers::WithinAbs(0.33, 1e-6));

    p2->deactivate(p2);
    p2->destroy(p2);
}

TEST_CASE("state: load before activate restores values on subsequent activate") {
    auto host = make_stub_host();

    // Save from an activated instance.
    const auto* p1 = create_plugin(&host);
    REQUIRE(p1->init(p1));
    REQUIRE(p1->activate(p1, 48000.0, 1, 512));

    const auto* params1 = static_cast<const clap_plugin_params_t*>(
        p1->get_extension(p1, CLAP_EXT_PARAMS));
    const auto* state1  = static_cast<const clap_plugin_state_t*>(
        p1->get_extension(p1, CLAP_EXT_STATE));

    // Set is_playing = 1.0 via flush.
    clap_id playing_id{CLAP_INVALID_ID};
    for (uint32_t i = 0; i < params1->count(p1); ++i) {
        clap_param_info_t info{};
        params1->get_info(p1, i, &info);
        if (std::strcmp(info.name, "is_playing") == 0) { playing_id = info.id; break; }
    }
    REQUIRE(playing_id != CLAP_INVALID_ID);

    clap_event_param_value_t ev{};
    ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    ev.param_id = playing_id; ev.value = 1.0;
    ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1;

    std::vector<clap_event_param_value_t> evs = {ev};
    clap_input_events_t  in{};
    clap_output_events_t out{};
    in.ctx  = &evs;
    in.size = [](const clap_input_events_t* q) -> uint32_t {
        return static_cast<uint32_t>(
            static_cast<const std::vector<clap_event_param_value_t>*>(q->ctx)->size());
    };
    in.get  = [](const clap_input_events_t* q, uint32_t i) -> const clap_event_header_t* {
        return &(*static_cast<const std::vector<clap_event_param_value_t>*>(q->ctx))[i].header;
    };
    out.ctx      = nullptr;
    out.try_push = [](const clap_output_events_t*, const clap_event_header_t*) -> bool { return true; };
    params1->flush(p1, &in, &out);

    MemOStream os;
    auto cs = os.clap_stream();
    REQUIRE(state1->save(p1, &cs));
    p1->deactivate(p1);
    p1->destroy(p1);

    // Load into a new instance BEFORE activate.
    const auto* p2 = create_plugin(&host);
    REQUIRE(p2->init(p2));

    const auto* state2 = static_cast<const clap_plugin_state_t*>(
        p2->get_extension(p2, CLAP_EXT_STATE));
    MemIStream is(os.buf);
    auto cis = is.clap_stream();
    REQUIRE(state2->load(p2, &cis));

    // Now activate — should preserve loaded values (not zero them).
    REQUIRE(p2->activate(p2, 48000.0, 1, 512));

    const auto* params2 = static_cast<const clap_plugin_params_t*>(
        p2->get_extension(p2, CLAP_EXT_PARAMS));
    double v{};
    REQUIRE(params2->get_value(p2, playing_id, &v));
    REQUIRE_THAT(v, Catch::Matchers::WithinAbs(1.0, 1e-6));

    p2->deactivate(p2);
    p2->destroy(p2);
}

TEST_CASE("state: load from truncated stream returns false") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* state = static_cast<const clap_plugin_state_t*>(
        p->get_extension(p, CLAP_EXT_STATE));

    // Only 3 bytes — too short for even the magic header.
    std::vector<uint8_t> garbage{0x01, 0x02, 0x03};
    MemIStream is(garbage);
    auto cis = is.clap_stream();
    REQUIRE_FALSE(state->load(p, &cis));

    p->deactivate(p);
    p->destroy(p);
}

TEST_CASE("state: load from wrong magic returns false") {
    auto host = make_stub_host();
    const auto* p = create_plugin(&host);
    REQUIRE(p->init(p));
    REQUIRE(p->activate(p, 48000.0, 1, 512));

    const auto* state = static_cast<const clap_plugin_state_t*>(
        p->get_extension(p, CLAP_EXT_STATE));

    // Write 12 bytes: wrong magic + correct version + 0 params.
    std::vector<uint8_t> bad(12, 0);
    bad[0] = 0xDE; bad[1] = 0xAD; bad[2] = 0xBE; bad[3] = 0xEF;  // bad magic
    bad[4] = 0x01;                                                   // version = 1
    // rest = 0

    MemIStream is(bad);
    auto cis = is.clap_stream();
    REQUIRE_FALSE(state->load(p, &cis));

    p->deactivate(p);
    p->destroy(p);
}
