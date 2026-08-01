// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <kairos_grid/vcv_bridge/bridge_frame.hpp>
#include <kairos_grid/vcv_bridge/shm_ring_buffer.hpp>
#include <kairos_grid/vcv_bridge/vcv_bridge_module.hpp>

#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace kairos_grid;
using namespace kairos_grid::vcv_bridge;

// ---- Helpers ---------------------------------------------------------------

static std::string shm_base(const char* tag) {
    return std::string("kg-vcb-test-") + tag;
}

// Fake VCVRack side: producer on <base>-in, consumer on <base>-out.
// Must be created BEFORE VCVBridgeModule (which attaches as consumer to -in).
struct FakeVCVRack {
    ShmRingBuffer in_prod;  // writes audio TO kairos
    ShmRingBuffer out_cons; // reads audio FROM kairos (attached after module is up)

    explicit FakeVCVRack(const std::string& base)
        : in_prod(ShmRingBuffer::create_producer("/" + base + "-in")),
          out_cons(ShmRingBuffer::create_consumer("/" + base + "-out")) {}
};

// Convenience: attach only after VCVBridgeModule has been constructed.
static ShmRingBuffer attach_out_consumer(const std::string& base) {
    return ShmRingBuffer::create_consumer("/" + base + "-out");
}

// Drive `n_samples` worth of `module.process()` calls with the given input voltages.
// `in_voltages[ch]` has length n_samples; results are read from module.outputs[].
static std::vector<std::vector<float>> drive(VCVBridgeModule& module, int n_channels, int n_samples,
                                             const std::vector<std::vector<float>>& in_voltages) {
    GridProcessArgs                 args{44100.f, 1.f / 44100.f, 0};
    std::vector<std::vector<float>> out(static_cast<std::size_t>(n_channels),
                                        std::vector<float>(static_cast<std::size_t>(n_samples)));
    for (int s = 0; s < n_samples; ++s) {
        for (int ch = 0; ch < n_channels; ++ch)
            module.inputs[static_cast<std::size_t>(ch)].voltage =
                in_voltages[static_cast<std::size_t>(ch)][static_cast<std::size_t>(s)];
        module.process(args);
        for (int ch = 0; ch < n_channels; ++ch)
            out[static_cast<std::size_t>(ch)][static_cast<std::size_t>(s)] =
                module.outputs[static_cast<std::size_t>(ch)].voltage;
        ++args.frame;
    }
    return out;
}

// ---- Construction tests ----------------------------------------------------

TEST_CASE("VCVBridgeModule construction — shm_out valid when created first",
          "[vcv_bridge][module]") {
    const auto base = shm_base("ctor");
    // Cleanup any stale segments first.
    ::shm_unlink(("/" + base + "-in").c_str());
    ::shm_unlink(("/" + base + "-out").c_str());

    // Module creates shm_out as producer unconditionally.
    VCVBridgeModule mod(base, 2, 4);
    REQUIRE(mod.shm_out_valid());
    // shm_in is not valid yet — VCVRack hasn't started.
    REQUIRE_FALSE(mod.shm_in_valid());
    REQUIRE(mod.n_channels() == 2);
    REQUIRE(mod.block_size() == 4);
    REQUIRE(mod.inputs.size() == 2u);
    REQUIRE(mod.outputs.size() == 2u);
}

TEST_CASE("VCVBridgeModule construction — shm_in valid when VCVRack creates segment first",
          "[vcv_bridge][module]") {
    const auto base = shm_base("ctor2");
    ::shm_unlink(("/" + base + "-in").c_str());
    ::shm_unlink(("/" + base + "-out").c_str());

    // VCVRack side: create shm_in producer before module.
    auto vcv_in = ShmRingBuffer::create_producer("/" + base + "-in");
    REQUIRE(vcv_in.valid());

    VCVBridgeModule mod(base, 2, 4);
    REQUIRE(mod.shm_in_valid());
    REQUIRE(mod.shm_out_valid());
}

// ---- No-crash when not connected -------------------------------------------

TEST_CASE("VCVBridgeModule process — outputs silence when shm_in not connected",
          "[vcv_bridge][module]") {
    const auto base = shm_base("silence");
    ::shm_unlink(("/" + base + "-in").c_str());
    ::shm_unlink(("/" + base + "-out").c_str());

    VCVBridgeModule mod(base, 2, 4);
    REQUIRE_FALSE(mod.shm_in_valid());

    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    // Should not crash even without shm_in.
    for (int i = 0; i < 8; ++i) {
        mod.process(args);
        REQUIRE(mod.outputs[0].voltage == 0.f);
        REQUIRE(mod.outputs[1].voltage == 0.f);
        ++args.frame;
    }
}

// ---- Audio round-trip (single block) ---------------------------------------

TEST_CASE("VCVBridgeModule audio round-trip — VCVRack audio delivered to kairos outputs",
          "[vcv_bridge][module]") {
    constexpr int kChannels  = 2;
    constexpr int kBlockSize = 8;

    const auto base = shm_base("roundtrip");
    ::shm_unlink(("/" + base + "-in").c_str());
    ::shm_unlink(("/" + base + "-out").c_str());

    // VCVRack creates shm_in first.
    auto vcv_in = ShmRingBuffer::create_producer("/" + base + "-in");
    REQUIRE(vcv_in.valid());

    // Build a known audio frame with a ramp on ch0 and a constant on ch1.
    BridgeFrame* frame       = vcv_in.writable_slot();
    frame->block_size        = kBlockSize;
    frame->n_audio_to_kairos = kChannels;
    for (int s = 0; s < kBlockSize; ++s) {
        frame->audio_to_kairos[0][s] = static_cast<float>(s) / (kBlockSize - 1);
        frame->audio_to_kairos[1][s] = -0.5f;
    }
    vcv_in.commit();

    // Construct module (attaches to shm_in, creates shm_out).
    VCVBridgeModule mod(base, kChannels, kBlockSize);
    REQUIRE(mod.shm_in_valid());

    // Attach VCVRack consumer for shm_out.
    auto vcv_out = attach_out_consumer(base);
    REQUIRE(vcv_out.valid());

    // Drive one full block (no external cables — inputs stay 0).
    const std::vector<std::vector<float>> in_vols(kChannels, std::vector<float>(kBlockSize, 0.f));
    const auto                            outs = drive(mod, kChannels, kBlockSize, in_vols);

    // Module outputs should mirror what VCVRack sent.
    for (int s = 0; s < kBlockSize; ++s) {
        REQUIRE(outs[0][static_cast<std::size_t>(s)] ==
                Catch::Approx(static_cast<float>(s) / (kBlockSize - 1)));
        REQUIRE(outs[1][static_cast<std::size_t>(s)] == Catch::Approx(-0.5f));
    }
}

TEST_CASE("VCVBridgeModule audio round-trip — kairos audio committed to shm_out",
          "[vcv_bridge][module]") {
    constexpr int kChannels  = 2;
    constexpr int kBlockSize = 8;

    const auto base = shm_base("kairos2vcv");
    ::shm_unlink(("/" + base + "-in").c_str());
    ::shm_unlink(("/" + base + "-out").c_str());

    // VCVRack creates shm_in with a silent frame (so module is in valid state).
    auto         vcv_in        = ShmRingBuffer::create_producer("/" + base + "-in");
    BridgeFrame* in_slot       = vcv_in.writable_slot();
    in_slot->block_size        = kBlockSize;
    in_slot->n_audio_to_kairos = kChannels;
    std::memset(in_slot->audio_to_kairos, 0, sizeof(in_slot->audio_to_kairos));
    vcv_in.commit();

    VCVBridgeModule mod(base, kChannels, kBlockSize);
    auto            vcv_out = attach_out_consumer(base);

    // Drive the module with a known input signal (kairos graph → VCVRack).
    // inputs[0] = ramp, inputs[1] = constant.
    std::vector<std::vector<float>> in_vols(kChannels, std::vector<float>(kBlockSize));
    for (int s = 0; s < kBlockSize; ++s) {
        in_vols[0][static_cast<std::size_t>(s)] = static_cast<float>(s) * 0.1f;
        in_vols[1][static_cast<std::size_t>(s)] = 0.75f;
    }
    drive(mod, kChannels, kBlockSize, in_vols);

    // After one full block, shm_out should have the committed frame.
    const BridgeFrame* out_frame = vcv_out.consume();
    REQUIRE(out_frame != nullptr);
    REQUIRE(out_frame->magic == kBridgeMagic);
    REQUIRE(out_frame->sequence == 1u);
    REQUIRE(out_frame->block_size == static_cast<uint32_t>(kBlockSize));

    for (int s = 0; s < kBlockSize; ++s) {
        REQUIRE(out_frame->audio_to_vcv[0][s] ==
                Catch::Approx(in_vols[0][static_cast<std::size_t>(s)]));
        REQUIRE(out_frame->audio_to_vcv[1][s] ==
                Catch::Approx(in_vols[1][static_cast<std::size_t>(s)]));
    }
}

// ---- Multi-block continuity ------------------------------------------------

TEST_CASE("VCVBridgeModule multi-block — sequence increments each block", "[vcv_bridge][module]") {
    constexpr int kChannels  = 1;
    constexpr int kBlockSize = 4;
    constexpr int kBlocks    = 5;

    const auto base = shm_base("multiblock");
    ::shm_unlink(("/" + base + "-in").c_str());
    ::shm_unlink(("/" + base + "-out").c_str());

    auto vcv_in = ShmRingBuffer::create_producer("/" + base + "-in");
    // Write a single frame; the module will reuse it for every block.
    BridgeFrame* inf       = vcv_in.writable_slot();
    inf->block_size        = kBlockSize;
    inf->n_audio_to_kairos = kChannels;
    for (int s = 0; s < kBlockSize; ++s)
        inf->audio_to_kairos[0][s] = static_cast<float>(s);
    vcv_in.commit();

    VCVBridgeModule mod(base, kChannels, kBlockSize);
    auto            vcv_out = attach_out_consumer(base);

    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    for (int b = 0; b < kBlocks; ++b) {
        for (int s = 0; s < kBlockSize; ++s) {
            mod.inputs[0].voltage = 0.f;
            mod.process(args);
            ++args.frame;
        }
        const BridgeFrame* f = vcv_out.consume();
        REQUIRE(f != nullptr);
        REQUIRE(f->sequence == static_cast<uint32_t>(b + 1));
    }
}

// ---- Concurrent: producer thread / module thread ---------------------------

TEST_CASE("VCVBridgeModule concurrent — 1000 blocks, no data races",
          "[vcv_bridge][module][concurrent]") {
    constexpr int kChannels  = 2;
    constexpr int kBlockSize = 16;
    constexpr int kBlocks    = 1000;

    const auto base = shm_base("concurrent");
    ::shm_unlink(("/" + base + "-in").c_str());
    ::shm_unlink(("/" + base + "-out").c_str());

    auto         vcv_in            = ShmRingBuffer::create_producer("/" + base + "-in");
    BridgeFrame* first_frame       = vcv_in.writable_slot();
    first_frame->block_size        = kBlockSize;
    first_frame->n_audio_to_kairos = kChannels;
    for (int s = 0; s < kBlockSize; ++s)
        for (int ch = 0; ch < kChannels; ++ch)
            first_frame->audio_to_kairos[ch][s] = static_cast<float>(s) * 0.01f;
    vcv_in.commit();

    VCVBridgeModule mod(base, kChannels, kBlockSize);
    auto            vcv_out = attach_out_consumer(base);

    // VCVRack thread: continuously writes new in frames.
    std::atomic<bool>     producer_done{false};
    std::atomic<uint32_t> produced{0};
    std::thread           producer_thread([&] {
        for (int b = 0; b < kBlocks; ++b) {
            BridgeFrame* f       = vcv_in.writable_slot();
            f->block_size        = kBlockSize;
            f->n_audio_to_kairos = kChannels;
            f->sequence          = static_cast<uint32_t>(b + 2);
            for (int s = 0; s < kBlockSize; ++s)
                for (int ch = 0; ch < kChannels; ++ch)
                    f->audio_to_kairos[ch][s] = static_cast<float>(b + s) * 0.001f;
            vcv_in.commit();
            produced.fetch_add(1, std::memory_order_relaxed);
        }
        producer_done.store(true, std::memory_order_release);
    });

    // kairos thread: drives the module for kBlocks blocks.
    GridProcessArgs args{48000.f, 1.f / 48000.f, 0};
    uint32_t        last_out_seq = 0;
    for (int b = 0; b < kBlocks; ++b) {
        for (int s = 0; s < kBlockSize; ++s) {
            for (int ch = 0; ch < kChannels; ++ch)
                mod.inputs[static_cast<std::size_t>(ch)].voltage =
                    static_cast<float>(b * kBlockSize + s) * 0.001f;
            mod.process(args);
            ++args.frame;
        }
        const BridgeFrame* f = vcv_out.consume();
        if (f) {
            REQUIRE(f->sequence >= last_out_seq);
            last_out_seq = f->sequence;
        }
    }

    producer_thread.join();

    REQUIRE(produced.load() == static_cast<uint32_t>(kBlocks));
    REQUIRE(last_out_seq == static_cast<uint32_t>(kBlocks));
}
