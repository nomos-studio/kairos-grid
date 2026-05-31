// SPDX-License-Identifier: GPL-3.0-or-later
//
// WasmGridModule smoke tests.
//
// These tests exercise error paths that require no real .wasm file.
// Integration tests (actual Faust-compiled WASM) are deferred until a
// test fixture .wasm is vendored into the tree.

#include <kairos_grid/wasm_grid_module.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

using namespace kairos_grid;

TEST_CASE("WasmGridModule::create returns nullptr for empty path", "[wasm]") {
    auto m = WasmGridModule::create("");
    REQUIRE(m == nullptr);
}

TEST_CASE("WasmGridModule::create returns nullptr for nonexistent file", "[wasm]") {
    auto m = WasmGridModule::create("/nonexistent/path/that/does/not/exist.wasm");
    REQUIRE(m == nullptr);
}

TEST_CASE("WasmGridModule::create returns nullptr for invalid wasm bytes", "[wasm]") {
    // Write a file with garbage bytes that are not valid WASM.
    const char* path = "/tmp/kairos_grid_test_invalid.wasm";
    if (FILE* f = std::fopen(path, "wb")) {
        const char garbage[] = "this is not a wasm file\n";
        std::fwrite(garbage, 1, sizeof(garbage) - 1, f);
        std::fclose(f);
    }
    auto m = WasmGridModule::create(path);
    REQUIRE(m == nullptr);
    std::remove(path);
}
