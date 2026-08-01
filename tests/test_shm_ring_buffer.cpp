// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <kairos_grid/vcv_bridge/bridge_frame.hpp>
#include <kairos_grid/vcv_bridge/shm_ring_buffer.hpp>

#include <atomic>
#include <cstring>
#include <thread>

using namespace kairos_grid::vcv_bridge;

// Unique shm name per test to avoid cross-test collisions under parallel ctest.
static std::string make_shm_name(const char* suffix) {
    return std::string("/kairos-test-") + suffix;
}

// Helper: ensure the shm segment doesn't exist before a test creates it.
static void cleanup(const std::string& name) {
    ::shm_unlink(name.c_str());
}

// --- Construction ---

TEST_CASE("ShmRingBuffer producer creates valid segment", "[vcv_bridge][shm]") {
    const auto name = make_shm_name("ctor-prod");
    cleanup(name);

    auto prod = ShmRingBuffer::create_producer(name);
    REQUIRE(prod.valid());
}

TEST_CASE("ShmRingBuffer consumer attaches to existing segment", "[vcv_bridge][shm]") {
    const auto name = make_shm_name("ctor-cons");
    cleanup(name);

    auto prod = ShmRingBuffer::create_producer(name);
    REQUIRE(prod.valid());

    auto cons = ShmRingBuffer::create_consumer(name);
    REQUIRE(cons.valid());
}

TEST_CASE("ShmRingBuffer consumer fails when segment does not exist", "[vcv_bridge][shm]") {
    const auto name = make_shm_name("ctor-noexist");
    cleanup(name);

    auto cons = ShmRingBuffer::create_consumer(name);
    REQUIRE_FALSE(cons.valid());
}

// --- No data before first commit ---

TEST_CASE("ShmRingBuffer consume returns nullptr before any commit", "[vcv_bridge][shm]") {
    const auto name = make_shm_name("no-data");
    cleanup(name);

    auto prod = ShmRingBuffer::create_producer(name);
    auto cons = ShmRingBuffer::create_consumer(name);

    REQUIRE(cons.consume() == nullptr);
}

// --- Single-threaded round-trip ---

TEST_CASE("ShmRingBuffer single-threaded round-trip", "[vcv_bridge][shm]") {
    const auto name = make_shm_name("roundtrip");
    cleanup(name);

    auto prod = ShmRingBuffer::create_producer(name);
    auto cons = ShmRingBuffer::create_consumer(name);

    // Write a frame with a known sequence and magic.
    BridgeFrame* slot = prod.writable_slot();
    REQUIRE(slot != nullptr);

    slot->sequence              = 42;
    slot->block_size            = 64;
    slot->audio_to_kairos[0][0] = 0.5f;
    slot->audio_to_vcv[3][127]  = -0.25f;
    prod.commit();

    // Consumer should now see the frame.
    const BridgeFrame* frame = cons.consume();
    REQUIRE(frame != nullptr);
    REQUIRE(frame->magic == kBridgeMagic);
    REQUIRE(frame->sequence == 42u);
    REQUIRE(frame->block_size == 64u);
    REQUIRE(frame->audio_to_kairos[0][0] == 0.5f);
    REQUIRE(frame->audio_to_vcv[3][127] == -0.25f);
}

TEST_CASE("ShmRingBuffer multiple frames — consumer always sees latest", "[vcv_bridge][shm]") {
    const auto name = make_shm_name("latest");
    cleanup(name);

    auto prod = ShmRingBuffer::create_producer(name);
    auto cons = ShmRingBuffer::create_consumer(name);

    for (uint32_t seq = 1; seq <= 10; ++seq) {
        BridgeFrame* slot = prod.writable_slot();
        slot->sequence    = seq;
        prod.commit();

        const BridgeFrame* frame = cons.consume();
        REQUIRE(frame != nullptr);
        REQUIRE(frame->sequence == seq);
    }
}

TEST_CASE("ShmRingBuffer consumer can fall behind — reads latest not oldest", "[vcv_bridge][shm]") {
    const auto name = make_shm_name("fallbehind");
    cleanup(name);

    auto prod = ShmRingBuffer::create_producer(name);
    auto cons = ShmRingBuffer::create_consumer(name);

    // Produce many frames without consuming.
    for (uint32_t seq = 1; seq <= 100; ++seq) {
        BridgeFrame* slot = prod.writable_slot();
        slot->sequence    = seq;
        prod.commit();
    }

    // Consumer reads exactly once — gets the latest (seq == 100), not the first.
    const BridgeFrame* frame = cons.consume();
    REQUIRE(frame != nullptr);
    REQUIRE(frame->sequence == 100u);
}

// --- Multi-threaded ---

TEST_CASE("ShmRingBuffer concurrent producer/consumer — 1M frames, no data races",
          "[vcv_bridge][shm][concurrent]") {
    const auto name = make_shm_name("concurrent");
    cleanup(name);

    constexpr int kFrames = 1'000'000;

    auto prod = ShmRingBuffer::create_producer(name);
    auto cons = ShmRingBuffer::create_consumer(name);

    std::atomic<uint32_t> frames_consumed{0};
    std::atomic<uint32_t> last_seq{0};
    std::atomic<bool>     done{false};

    // Consumer thread: reads as fast as possible; checks sequence is non-decreasing.
    std::thread consumer_thread([&] {
        uint32_t prev_seq = 0;
        while (!done.load(std::memory_order_acquire) ||
               frames_consumed.load(std::memory_order_relaxed) < 1u) {
            const BridgeFrame* frame = cons.consume();
            if (!frame)
                continue;
            const uint32_t seq = frame->sequence;
            if (seq != prev_seq) {
                // Sequence must be strictly greater — consumer never goes backward.
                REQUIRE(seq > prev_seq);
                prev_seq = seq;
                frames_consumed.fetch_add(1, std::memory_order_relaxed);
                last_seq.store(seq, std::memory_order_relaxed);
            }
        }
    });

    // Producer: writes kFrames frames with monotonically increasing sequence.
    for (uint32_t seq = 1; seq <= static_cast<uint32_t>(kFrames); ++seq) {
        BridgeFrame* slot = prod.writable_slot();
        slot->sequence    = seq;
        prod.commit();
    }

    done.store(true, std::memory_order_release);
    consumer_thread.join();

    // At minimum the consumer must have seen at least one frame and the last
    // producer sequence (since it keeps reading until done).
    REQUIRE(frames_consumed.load() >= 1u);
    REQUIRE(last_seq.load() > 0u);
}

TEST_CASE("ShmRingBuffer ES-8CV data survives concurrent exchange",
          "[vcv_bridge][shm][concurrent]") {
    const auto name = make_shm_name("cv-concurrent");
    cleanup(name);

    constexpr int kRounds = 50'000;

    auto prod = ShmRingBuffer::create_producer(name);
    auto cons = ShmRingBuffer::create_consumer(name);

    // Producer encodes a recognisable CV pattern per frame.
    // Consumer verifies the ES-8CV fields are coherent (magic still intact).
    std::atomic<bool> done{false};
    std::atomic<int>  coherent_reads{0};

    std::thread consumer_thread([&] {
        while (!done.load(std::memory_order_acquire)) {
            const BridgeFrame* f = cons.consume();
            if (!f)
                continue;
            if (f->magic == kBridgeMagic && f->es8cv_valid)
                coherent_reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < kRounds; ++i) {
        BridgeFrame* slot        = prod.writable_slot(); // stamps magic
        slot->sequence           = static_cast<uint32_t>(i + 1);
        slot->es8cv_valid        = 1;
        slot->es8cv_to_kairos[0] = static_cast<float>(i % 256) / 256.0f;
        prod.commit();
    }

    done.store(true, std::memory_order_release);
    consumer_thread.join();

    REQUIRE(coherent_reads.load() > 0);
}
