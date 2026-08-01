// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bridge_frame.hpp"

// ShmRingBuffer — cross-process SPSC double-buffer over POSIX shared memory.
//
// Topology: one producer, one consumer, both on the same host.
// The producer always wins: if the consumer is slow, it reads the latest frame.
// No queuing — stale frames are silently overwritten.
//
// Shared memory layout (total kShmTotalSize bytes):
//   [ShmHeader : 64 bytes]
//   [slot 0    : sizeof(BridgeFrame)]
//   [slot 1    : sizeof(BridgeFrame)]
//
// Protocol:
//   Producer holds producer_slot_ (0 or 1).  It fills slot[producer_slot_],
//   then stores producer_slot_ into write_idx (release), then flips producer_slot_.
//   Consumer loads write_idx (acquire); -1 means no data yet.
//
// Safety: std::atomic<int> must be ATOMIC_INT_LOCK_FREE == 2 (asserted at runtime).
// Both processes must compile from the same bridge_frame.hpp (identical layout).

static_assert(ATOMIC_INT_LOCK_FREE == 2,
              "std::atomic<int> must be lock-free for cross-process shared memory use");

namespace kairos_grid::vcv_bridge {

struct alignas(64) ShmHeader {
    std::atomic<int> write_idx{-1}; // -1 = no frame yet; 0|1 = latest slot
    char             _pad[64 - sizeof(std::atomic<int>)]{};
};
static_assert(sizeof(ShmHeader) == 64);

constexpr std::size_t kShmTotalSize = sizeof(ShmHeader) + 2 * sizeof(BridgeFrame);

class ShmRingBuffer {
  public:
    // Producer (owner): unlinks any stale segment, creates and initializes shm.
    // name must be a POSIX shm name starting with '/' (e.g. "/kairos-vcv-0").
    static ShmRingBuffer create_producer(const std::string& name) {
        return ShmRingBuffer(name, /*producer=*/true);
    }

    // Consumer: attaches to an existing shm segment created by the producer.
    static ShmRingBuffer create_consumer(const std::string& name) {
        return ShmRingBuffer(name, /*producer=*/false);
    }

    ~ShmRingBuffer() {
        if (mem_ && mem_ != MAP_FAILED)
            ::munmap(mem_, kShmTotalSize);
        if (fd_ >= 0)
            ::close(fd_);
        if (owner_)
            ::shm_unlink(name_.c_str());
    }

    ShmRingBuffer(const ShmRingBuffer&)            = delete;
    ShmRingBuffer& operator=(const ShmRingBuffer&) = delete;

    ShmRingBuffer(ShmRingBuffer&& o) noexcept
        : mem_(o.mem_), fd_(o.fd_), owner_(o.owner_), producer_slot_(o.producer_slot_),
          name_(std::move(o.name_)) {
        o.mem_   = nullptr;
        o.fd_    = -1;
        o.owner_ = false;
    }

    bool valid() const noexcept { return mem_ && mem_ != MAP_FAILED; }

    const std::string& name() const noexcept { return name_; }

    // --- Producer API ---

    // Returns the BridgeFrame the producer should fill next.
    // Returns nullptr if the ring buffer is not valid (construction failed).
    // Always stamps magic so consumers can sanity-check frames.
    BridgeFrame* writable_slot() noexcept {
        if (!mem_)
            return nullptr;
        BridgeFrame* slot = frame_at(producer_slot_);
        slot->magic       = kBridgeMagic;
        return slot;
    }

    // Atomically publish the frame written to writable_slot().
    // Flips the write slot so the next writable_slot() call returns the other one.
    // No-op if the ring buffer is not valid.
    void commit() noexcept {
        if (!mem_)
            return;
        hdr()->write_idx.store(producer_slot_, std::memory_order_release);
        producer_slot_ ^= 1;
    }

    // --- Consumer API ---

    // Returns a pointer to the latest committed frame, or nullptr if none yet
    // or if the ring buffer is not valid (construction failed / not connected).
    // Valid until the next call to consume() — do not hold across iterations.
    const BridgeFrame* consume() noexcept {
        if (!mem_)
            return nullptr;
        const int idx = hdr()->write_idx.load(std::memory_order_acquire);
        if (idx < 0)
            return nullptr;
        return frame_at(idx);
    }

  private:
    explicit ShmRingBuffer(const std::string& name, bool producer) : owner_(producer), name_(name) {

        if (producer)
            ::shm_unlink(name.c_str()); // clean up stale segment; ignore error

        const int flags = producer ? (O_CREAT | O_EXCL | O_RDWR) : O_RDWR;
        fd_             = ::shm_open(name.c_str(), flags, 0600);
        if (fd_ < 0)
            return;

        if (producer && ::ftruncate(fd_, static_cast<off_t>(kShmTotalSize)) != 0) {
            ::close(fd_);
            fd_ = -1;
            return;
        }

        mem_ = ::mmap(nullptr, kShmTotalSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mem_ == MAP_FAILED) {
            mem_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            return;
        }

        if (producer) {
            new (mem_) ShmHeader{};
            std::memset(static_cast<char*>(mem_) + sizeof(ShmHeader), 0, 2 * sizeof(BridgeFrame));
        }
    }

    ShmHeader* hdr() noexcept { return reinterpret_cast<ShmHeader*>(mem_); }

    BridgeFrame* frame_at(int idx) noexcept {
        auto* base = static_cast<char*>(mem_) + sizeof(ShmHeader);
        return reinterpret_cast<BridgeFrame*>(base + idx * sizeof(BridgeFrame));
    }

    void*       mem_{nullptr};
    int         fd_{-1};
    bool        owner_{false};
    int         producer_slot_{0};
    std::string name_;
};

} // namespace kairos_grid::vcv_bridge
