// SPDX-License-Identifier: GPL-3.0-or-later
// Concurrent-access tests for the CLAP plugin.
//
// These tests exercise the audio-thread / main-thread boundary under real
// concurrency and are the primary target for the tsan CI preset.  A clean TSan
// run confirms that the acquire/release synchronisation between process() and
// params_get_value() is correct.  Prior to the atomic param_frame_ fix
// (kairos-grid a6fc0ce) TSan would report a data race here.
//
// clap_plugin.cpp is compiled directly into kairos-grid-plugin-tests (see
// tests/CMakeLists.txt) so the factory and entry point are available without
// dlopen().

#include <clap/clap.h>
#include <kairos_grid/clap_kairos_patch_bus.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

namespace {

clap_host_t make_stub_host() {
  clap_host_t h{};
  h.clap_version = CLAP_VERSION_INIT;
  h.name = "test-host";
  h.vendor = "kairos-grid-tests";
  h.url = "";
  h.version = "0";
  h.get_extension = [](const clap_host_t *, const char *) -> const void * {
    return nullptr;
  };
  h.request_restart = [](const clap_host_t *) {};
  h.request_process = [](const clap_host_t *) {};
  h.request_callback = [](const clap_host_t *) {};
  return h;
}

extern "C" const clap_plugin_entry_t clap_entry;

const clap_plugin_t *create_plugin(const clap_host_t *host) {
  clap_entry.init("");
  const auto *factory = static_cast<const clap_plugin_factory_t *>(
      clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
  if (!factory)
    return nullptr;
  return factory->create_plugin(factory, host, "studio.nomos.kairos-grid");
}

// Minimal clap_process_t: silent stereo I/O, empty event queues, no transport.
struct SilentProcess {
  static constexpr uint32_t k_block = 64;

  std::vector<float> in_l, in_r, out_l, out_r;
  float *in_data[2];
  float *out_data[2];
  clap_audio_buffer_t audio_in{};
  clap_audio_buffer_t audio_out{};
  clap_input_events_t in_evts{};
  clap_output_events_t out_evts{};
  clap_process_t proc{};

  SilentProcess()
      : in_l(k_block, 0.f), in_r(k_block, 0.f), out_l(k_block, 0.f),
        out_r(k_block, 0.f) {
    in_data[0] = in_l.data();
    in_data[1] = in_r.data();
    out_data[0] = out_l.data();
    out_data[1] = out_r.data();

    audio_in.data32 = in_data;
    audio_in.channel_count = 2;
    audio_out.data32 = out_data;
    audio_out.channel_count = 2;

    in_evts.ctx = nullptr;
    in_evts.size = [](const clap_input_events_t *) -> uint32_t { return 0; };
    in_evts.get = [](const clap_input_events_t *,
                     uint32_t) -> const clap_event_header_t * {
      return nullptr;
    };

    out_evts.ctx = nullptr;
    out_evts.try_push = [](const clap_output_events_t *,
                           const clap_event_header_t *) -> bool {
      return true;
    };

    proc.frames_count = k_block;
    proc.transport = nullptr;
    proc.audio_inputs = &audio_in;
    proc.audio_inputs_count = 1;
    proc.audio_outputs = &audio_out;
    proc.audio_outputs_count = 1;
    proc.in_events = &in_evts;
    proc.out_events = &out_evts;
  }
};

// Minimal patch: env + audio-out, no cables.  Produces 7 EnvironmentModule
// params.
static constexpr const char *k_env_out_edn =
    "{:modules [{:type \"env\"} {:type \"audio-out\"}] :cables []}";

} // namespace

// ---------------------------------------------------------------------------
// Concurrent: process() vs params_get_value()
//
// Audio-thread contract: process() is the sole writer of param_frame_ and
// param_count_ (via try_install_pending_slot).  Main-thread contract:
// params_get_value() loads param_count_ with acquire, then reads param_frame_.
//
// Pushing a new patch via push_patch() queues a PatchSlot that
// try_install_pending_slot() consumes at the next process() call, driving a
// full param_count_/param_frame_ publish cycle on every swap.
// ---------------------------------------------------------------------------

TEST_CASE("concurrent: process() and params_get_value() from two threads") {
  auto host = make_stub_host();
  const auto *p = create_plugin(&host);
  REQUIRE(p != nullptr);
  REQUIRE(p->init(p));
  REQUIRE(p->activate(p, 48000.0, 1, SilentProcess::k_block));

  const auto *pb = static_cast<const clap_plugin_patch_bus_t *>(
      p->get_extension(p, CLAP_EXT_KAIROS_PATCH_BUS));
  const auto *params = static_cast<const clap_plugin_params_t *>(
      p->get_extension(p, CLAP_EXT_PARAMS));
  REQUIRE(pb != nullptr);
  REQUIRE(params != nullptr);

  // Install the initial patch so engine_.has_value() is true before the
  // audio thread starts.  One synchronous process() call consumes the slot.
  const uint32_t edn_len = static_cast<uint32_t>(std::strlen(k_env_out_edn));
  REQUIRE(pb->push_patch(p, k_env_out_edn, edn_len));
  REQUIRE(p->start_processing(p));
  {
    SilentProcess bootstrap;
    REQUIRE(p->process(p, &bootstrap.proc) != CLAP_PROCESS_ERROR);
  }
  REQUIRE(params->count(p) == 7);

  // -----------------------------------------------------------------------
  // Audio thread: calls process() in a tight loop.
  //
  // Main thread:  calls params_get_value() for every param on every
  //               iteration; pushes a fresh patch every 200 iterations to
  //               keep try_install_pending_slot() active across the window.
  // -----------------------------------------------------------------------

  constexpr int k_main_iters = 2000;

  std::atomic<bool> audio_running{true};

  std::thread audio_thread{[&]() {
    SilentProcess sp;
    while (audio_running.load(std::memory_order_relaxed))
      p->process(p, &sp.proc);
  }};

  for (int iter = 0; iter < k_main_iters; ++iter) {
    const uint32_t n = params->count(p);
    for (uint32_t i = 0; i < n; ++i) {
      double v{};
      params->get_value(p, static_cast<clap_id>(i), &v);
    }

    if (iter % 200 == 0)
      pb->push_patch(p, k_env_out_edn, edn_len);
  }

  audio_running.store(false, std::memory_order_relaxed);
  audio_thread.join();

  p->stop_processing(p);
  p->deactivate(p);
  p->destroy(p);
}
