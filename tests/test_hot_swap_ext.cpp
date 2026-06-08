// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hot-swap extension smoke tests.
//
// Tests the kairos/hot-swap CLAP extension on kairos-grid.
// These tests exercise error paths that require no real .wasm file.
// Integration tests (actual WASM swap with param migration) are deferred
// until a compiled test fixture is vendored into the tree.

#include <kairos_grid/clap_kairos_hot_swap.h>
#include <kairos_grid/clap_kairos_patch_bus.h>

#include <catch2/catch_test_macros.hpp>

#include <clap/clap.h>

#include <cstdio>
#include <cstring>

// Pull in the plugin factory via its C entry point.
extern "C" { extern const clap_plugin_entry_t clap_entry; }

static const clap_plugin_t* make_plugin() {
    clap_entry.init("");
    const auto* fac = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!fac) return nullptr;
    static const clap_host_t k_dummy_host = {
        .clap_version = CLAP_VERSION_INIT,
        .host_data    = nullptr,
        .name         = "test", .vendor = "", .url = "", .version = "0",
        .get_extension   = [](const clap_host_t*, const char*) -> const void* { return nullptr; },
        .request_restart = [](const clap_host_t*) {},
        .request_process = [](const clap_host_t*) {},
        .request_callback= [](const clap_host_t*) {},
    };
    const auto* p = fac->create_plugin(fac, &k_dummy_host, "studio.nomos.kairos-grid");
    if (!p) return nullptr;
    p->init(p);
    return p;
}

static void free_plugin(const clap_plugin_t* p) {
    if (p) p->destroy(p);
    clap_entry.deinit();
}

// ---------------------------------------------------------------------------

TEST_CASE("hot-swap extension is exposed when WASM is enabled", "[hot-swap]") {
    const clap_plugin_t* p = make_plugin();
    REQUIRE(p != nullptr);

    const auto* ext = static_cast<const clap_kairos_hot_swap_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_HOT_SWAP));
    REQUIRE(ext != nullptr);
    REQUIRE(ext->request != nullptr);

    free_plugin(p);
}

TEST_CASE("hot-swap request returns false for null path", "[hot-swap]") {
    const clap_plugin_t* p = make_plugin();
    REQUIRE(p != nullptr);

    const auto* ext = static_cast<const clap_kairos_hot_swap_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_HOT_SWAP));
    REQUIRE(ext != nullptr);

    REQUIRE_FALSE(ext->request(p, nullptr, nullptr));
    REQUIRE_FALSE(ext->request(p, "", nullptr));

    free_plugin(p);
}

TEST_CASE("hot-swap request returns false when no patch is loaded", "[hot-swap]") {
    // Write a file with plausible bytes so the readability check passes,
    // but no patch has been pushed yet so current_edn_ is empty.
    const char* path = "/tmp/kairos_grid_hot_swap_smoke.wasm";
    if (FILE* f = std::fopen(path, "wb")) {
        const char dummy[] = "dummy";
        std::fwrite(dummy, 1, sizeof(dummy) - 1, f);
        std::fclose(f);
    }

    const clap_plugin_t* p = make_plugin();
    REQUIRE(p != nullptr);

    const auto* ext = static_cast<const clap_kairos_hot_swap_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_HOT_SWAP));
    REQUIRE(ext != nullptr);

    // No patch pushed → current_edn_ is empty → request() returns false.
    REQUIRE_FALSE(ext->request(p, path, nullptr));

    std::remove(path);
    free_plugin(p);
}

TEST_CASE("hot-swap request returns false for nonexistent file", "[hot-swap]") {
    const clap_plugin_t* p = make_plugin();
    REQUIRE(p != nullptr);

    const auto* ext = static_cast<const clap_kairos_hot_swap_t*>(
        p->get_extension(p, CLAP_EXT_KAIROS_HOT_SWAP));
    REQUIRE(ext != nullptr);

    REQUIRE_FALSE(ext->request(p, "/nonexistent/path/that/does/not/exist.wasm", nullptr));

    free_plugin(p);
}
