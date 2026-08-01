// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <kairos_grid/vcv_bridge/es_codec.hpp>

using namespace kairos_grid::vcv_bridge;

// --- ES-8CV ---

TEST_CASE("ES-8CV round-trip — all zeros", "[vcv_bridge][es_codec]") {
    const uint16_t cv_in[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    float          encoded[4];
    uint16_t       cv_out[8];

    es8cv_encode(cv_in, encoded);
    es8cv_decode(encoded, cv_out);

    for (int i = 0; i < 8; ++i)
        REQUIRE(cv_out[i] == 0);
}

TEST_CASE("ES-8CV round-trip — all max (4095)", "[vcv_bridge][es_codec]") {
    const uint16_t cv_in[8] = {4095, 4095, 4095, 4095, 4095, 4095, 4095, 4095};
    float          encoded[4];
    uint16_t       cv_out[8];

    es8cv_encode(cv_in, encoded);
    es8cv_decode(encoded, cv_out);

    for (int i = 0; i < 8; ++i)
        REQUIRE(cv_out[i] == 4095);
}

TEST_CASE("ES-8CV round-trip — alternating 0/4095", "[vcv_bridge][es_codec]") {
    const uint16_t cv_in[8] = {0, 4095, 0, 4095, 0, 4095, 0, 4095};
    float          encoded[4];
    uint16_t       cv_out[8];

    es8cv_encode(cv_in, encoded);
    es8cv_decode(encoded, cv_out);

    for (int i = 0; i < 8; ++i)
        REQUIRE(cv_out[i] == cv_in[i]);
}

TEST_CASE("ES-8CV round-trip — alternating 4095/0", "[vcv_bridge][es_codec]") {
    const uint16_t cv_in[8] = {4095, 0, 4095, 0, 4095, 0, 4095, 0};
    float          encoded[4];
    uint16_t       cv_out[8];

    es8cv_encode(cv_in, encoded);
    es8cv_decode(encoded, cv_out);

    for (int i = 0; i < 8; ++i)
        REQUIRE(cv_out[i] == cv_in[i]);
}

TEST_CASE("ES-8CV round-trip — midpoint (2048)", "[vcv_bridge][es_codec]") {
    const uint16_t cv_in[8] = {2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048};
    float          encoded[4];
    uint16_t       cv_out[8];

    es8cv_encode(cv_in, encoded);
    es8cv_decode(encoded, cv_out);

    for (int i = 0; i < 8; ++i)
        REQUIRE(cv_out[i] == 2048);
}

TEST_CASE("ES-8CV round-trip — distinct values per channel", "[vcv_bridge][es_codec]") {
    const uint16_t cv_in[8] = {0, 512, 1023, 1024, 2048, 3071, 3072, 4095};
    float          encoded[4];
    uint16_t       cv_out[8];

    es8cv_encode(cv_in, encoded);
    es8cv_decode(encoded, cv_out);

    for (int i = 0; i < 8; ++i)
        REQUIRE(cv_out[i] == cv_in[i]);
}

TEST_CASE("ES-8CV round-trip — exhaustive 12-bit pairs", "[vcv_bridge][es_codec]") {
    // Verify that all 4096 possible 12-bit values round-trip exactly.
    // Tests two channels at a time to match the pair-packing layout.
    float    encoded[4];
    uint16_t cv_out[8];

    for (uint16_t hi = 0; hi < 16; ++hi) {
        for (uint16_t lo = 0; lo < 16; ++lo) {
            const uint16_t cv_hi    = hi * 256;
            const uint16_t cv_lo    = lo * 256;
            const uint16_t cv_in[8] = {cv_hi, cv_lo, 0, 0, 0, 0, 0, 0};

            es8cv_encode(cv_in, encoded);
            es8cv_decode(encoded, cv_out);

            REQUIRE(cv_out[0] == cv_hi);
            REQUIRE(cv_out[1] == cv_lo);
        }
    }
}

TEST_CASE("ES-8CV 12-bit mask — bits above 12 are ignored", "[vcv_bridge][es_codec]") {
    // Values with bits set above bit 11 must be treated as their masked equivalents.
    const uint16_t cv_in[8] = {0xF000u | 42u, 0xF000u | 1023u, 0, 0, 0, 0, 0, 0};
    float          encoded[4];
    uint16_t       cv_out[8];

    es8cv_encode(cv_in, encoded);
    es8cv_decode(encoded, cv_out);

    REQUIRE(cv_out[0] == 42);
    REQUIRE(cv_out[1] == 1023);
}

// --- ES-5 ---

TEST_CASE("ES-5 round-trip — all 256 gate byte values", "[vcv_bridge][es_codec]") {
    for (int g = 0; g <= 255; ++g) {
        const uint8_t gates = static_cast<uint8_t>(g);
        const float   f     = es5_encode(gates);
        const uint8_t back  = es5_decode(f);
        REQUIRE(back == gates);
    }
}

TEST_CASE("ES-5 encode range", "[vcv_bridge][es_codec]") {
    // All encoded values must be in [-1.0, 1.0).
    for (int g = 0; g <= 255; ++g) {
        const float f = es5_encode(static_cast<uint8_t>(g));
        REQUIRE(f >= -1.0f);
        REQUIRE(f < 1.0f);
    }
}

TEST_CASE("ES-5 decode — saturates on out-of-range input", "[vcv_bridge][es_codec]") {
    REQUIRE(es5_decode(-2.0f) == 0u);
    REQUIRE(es5_decode(2.0f) == 255u);
    REQUIRE(es5_decode(-10.0f) == 0u);
    REQUIRE(es5_decode(10.0f) == 255u);
}

TEST_CASE("ES-5 encode — known values", "[vcv_bridge][es_codec]") {
    // 0x00 → -1.0, 0x80 (128) → 0.0, 0xFF (255) → (255-128)/128
    REQUIRE(es5_encode(0x00) == Catch::Approx(-1.0f));
    REQUIRE(es5_encode(0x80) == Catch::Approx(0.0f));
    REQUIRE(es5_encode(0xFF) == Catch::Approx(127.0f / 128.0f));
}
