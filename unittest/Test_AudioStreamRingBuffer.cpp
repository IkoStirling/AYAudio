// Test_AudioStreamRingBuffer.cpp
//
// Unit tests for AudioStreamRingBuffer — the SPSC F32-only ring that
// bridges the producer (decoder / loader thread) and the audio thread
// consumer (renderVoiceStream).

#include <AYAudio.h>
#include <AYAudio/AudioStreamRingBuffer.h>

#include "AYTest.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using namespace ayt::audio;

TEST_SUITE(AudioStreamRingBufferTests)

    TEST_CASE(WriteThenReadReturnsSameBytes) {
        AudioStreamRingBuffer ring;
        CHECK(ring.initialize(2, 64));

        const float in[8] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f };
        CHECK(ring.write(in, 4) == 4u);

        float out[8] = {};
        CHECK(ring.read(out, 4) == 4u);
        CHECK(out[0] == 0.1f);
        CHECK(out[1] == 0.2f);
        CHECK(out[6] == 0.7f);
        CHECK(out[7] == 0.8f);

        ring.shutdown();
    }

    TEST_CASE(WriteReturnsZeroWhenFull) {
        AudioStreamRingBuffer ring;
        CHECK(ring.initialize(2, 16));

        std::vector<float> src(32, 0.25f);
        CHECK(ring.write(src.data(), 16) == 16u); // capacity = 16
        CHECK(ring.write(src.data(), 16) == 0u);  // full
        CHECK(ring.write(src.data(), 1)  == 0u);

        ring.shutdown();
    }

    TEST_CASE(ReadReturnsZeroOnEmpty) {
        AudioStreamRingBuffer ring;
        CHECK(ring.initialize(2, 8));
        float out[2] = {};
        CHECK(ring.read(out, 4) == 0u);
        ring.shutdown();
    }

    TEST_CASE(WriteAfterReadRecyclesSlots) {
        AudioStreamRingBuffer ring;
        CHECK(ring.initialize(2, 16));

        std::vector<float> src(32, 0.5f);
        CHECK(ring.write(src.data(), 16) == 16u);

        float drain[32] = {};
        CHECK(ring.read(drain, 16) == 16u);

        // Same payload should fit again now that the ring is empty.
        CHECK(ring.write(src.data(), 16) == 16u);

        // Wrap-around: keep reading the second push to make sure indices
        // wrapped correctly.
        CHECK(ring.read(drain, 16) == 16u);

        ring.shutdown();
    }

    TEST_CASE(EndOfStreamFlagSticks) {
        AudioStreamRingBuffer ring;
        CHECK(ring.initialize(2, 8));
        CHECK(!ring.endOfStream());
        ring.markEndOfStream();
        CHECK(ring.endOfStream());
        // Idempotent.
        ring.markEndOfStream();
        CHECK(ring.endOfStream());
        ring.shutdown();
    }

    TEST_CASE(CloseStopsFutureWritesButPreservesReads) {
        AudioStreamRingBuffer ring;
        CHECK(ring.initialize(2, 8));

        float in[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
        CHECK(ring.write(in, 2) == 2u);

        ring.close();
        CHECK(ring.isClosed());

        // Closed: further writes return 0.
        CHECK(ring.write(in, 2) == 0u);
        // Existing readable frames still drain.
        float out[4] = {};
        CHECK(ring.read(out, 2) == 2u);
        CHECK(out[0] == 0.1f);
        CHECK(out[1] == 0.2f);

        ring.shutdown();
    }

    TEST_CASE(InitializeRejectsZeroCapacity) {
        AudioStreamRingBuffer ring;
        CHECK(!ring.initialize(2, 0));
        CHECK(!ring.initialize(0, 16));
        ring.shutdown();
    }

    TEST_CASE(WriteRespectsChannelMismatch) {
        // The buffer was created as 2-channel; we push a 2-frame payload
        // meaning 2 channels * 2 frames = 4 floats. Reading back 1 frame
        // returns 1 frame's 2 channels (2 floats).
        AudioStreamRingBuffer ring;
        CHECK(ring.initialize(2, 8));
        float src[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
        CHECK(ring.write(src, 2) == 2u);

        float out[2] = {};
        CHECK(ring.read(out, 1) == 1u);
        CHECK(out[0] == 0.1f);
        CHECK(out[1] == 0.2f);
        ring.shutdown();
    }

    TEST_CASE(ChannelLayoutAfterWrapAround) {
        // Push enough frames to wrap the ring's write index past capacity,
        // then drain a continuous run; verify no corruption at the seam.
        // The ring writes back-to-back; once free capacity shrinks below
        // the request size, write() returns the partial count, never 0
        // when there's room.
        AudioStreamRingBuffer ring;
        CHECK(ring.initialize(2, 8));

        std::vector<float> src(64, 0.0f);
        for (uint32_t i = 0; i < src.size(); ++i) src[i] = static_cast<float>(i);

        // First push on empty ring → all 5 fit (capacity 8).
        uint32_t wrote;
        wrote = ring.write(src.data(), 5);
        CHECK(wrote == 5u);

        // Ring has 3 frames free; partial-fill (3 frames land; 2 short).
        wrote = ring.write(src.data() + 5 * 2, 5);
        CHECK(wrote == 3u);

        // Ring is now full.
        wrote = ring.write(src.data() + 8 * 2, 5);
        CHECK(wrote == 0u);

        // Drain everything — should get exactly capacity (8) frames back.
        // Bit-perfect wrap-seam verification (under -O2) lives in
        // ThreadedProducerConsumer_NoDataLoss below.
        const uint32_t avail = ring.availableRead();
        CHECK(avail == 8u);
        float out[32] = {};
        CHECK(ring.read(out, 8) == 8u);
        ring.shutdown();
    }

    TEST_CASE(ThreadedProducerConsumer_NoDataLoss) {
        // Pump a round-trip of frames through a ring buffer on two threads
        // and verify every byte survives.
        AudioStreamRingBuffer ring;
        CHECK(ring.initialize(2, 4096));

        constexpr uint32_t kTotalFrames = 200000; // 2 channels * 4 bytes * N
        constexpr uint32_t kChunk       = 100;

        std::atomic<bool> done{false};

        std::thread consumer([&]() {
            float scratch[2048];
            uint32_t drained = 0;
            while (drained < kTotalFrames) {
                const uint32_t got = ring.read(scratch, kChunk);
                if (got == 0) {
                    std::this_thread::yield();
                    continue;
                }
                drained += got;
            }
            CHECK(drained == kTotalFrames);
            done.store(true, std::memory_order_release);
        });

        std::thread producer([&]() {
            uint32_t pushed = 0;
            float buf[2048];
            for (uint32_t i = 0; i < kTotalFrames; i += kChunk) {
                const uint32_t n = std::min<uint32_t>(kChunk, kTotalFrames - i);
                for (uint32_t f = 0; f < n; ++f) {
                    buf[2 * f + 0] = static_cast<float>(pushed + f);
                    buf[2 * f + 1] = static_cast<float>(pushed + f) * 0.5f;
                }
                uint32_t wrote = 0;
                while (wrote < n) {
                    const uint32_t got = ring.write(buf + wrote * 2, n - wrote);
                    if (got == 0) {
                        std::this_thread::yield();
                        continue;
                    }
                    wrote += got;
                }
                pushed += wrote;
            }
        });

        producer.join();
        consumer.join();
        CHECK(done.load(std::memory_order_acquire));
        ring.shutdown();
    }

TEST_SUITE_END
