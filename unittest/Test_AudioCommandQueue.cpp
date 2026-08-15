#include <AYResource/assetsImpl/Audio.h>
#include "AYTest.h"

using namespace ayt::audio;

TEST_SUITE(AudioCommandQueueTests)

    TEST_CASE(InitializeRequiresNonZeroCapacity) {
        AudioCommandQueue q;
        CHECK(!q.initialize(0));
        CHECK(q.initialize(16));
        CHECK(q.isInitialized());
        CHECK_INT_EQ(q.capacity(), 16u);
        q.shutdown();
    }

    TEST_CASE(InitializeRoundsToPowerOfTwo) {
        AudioCommandQueue q;
        q.initialize(20);
        CHECK_INT_EQ(q.capacity(), 32u); // rounded up
        q.shutdown();
    }

    TEST_CASE(PushPopRoundTripsCommands) {
        AudioCommandQueue q;
        q.initialize(8);

        AudioCommand in{};
        in.type = AudioCommandType::SetBusGain;
        in.bus = AudioBus::Music;
        in.scalar = 0.5f;
        CHECK(q.tryPush(in));

        AudioCommand out{};
        CHECK(q.tryPop(out));
        CHECK(out.type == AudioCommandType::SetBusGain);
        CHECK(out.bus == AudioBus::Music);
        CHECK_FLOAT_EQ(out.scalar, 0.5f, 1e-6f);

        AudioCommand none{};
        CHECK(!q.tryPop(none)); // empty
        q.shutdown();
    }

    TEST_CASE(FullQueueRejectsPush) {
        AudioCommandQueue q;
        q.initialize(4); // capacity 4

        AudioCommand cmd{};
        cmd.type = AudioCommandType::StopAll;
        for (int i = 0; i < 4; ++i) {
            CHECK(q.tryPush(cmd));
        }
        CHECK(!q.tryPush(cmd)); // full
        CHECK_INT_EQ(q.approximateDepth(), 4u);
        q.shutdown();
    }

    TEST_CASE(DepthDecreasesAfterPops) {
        AudioCommandQueue q;
        q.initialize(8);

        AudioCommand cmd{};
        cmd.type = AudioCommandType::StopAll;
        for (int i = 0; i < 5; ++i) q.tryPush(cmd);
        CHECK_INT_EQ(q.approximateDepth(), 5u);

        AudioCommand out{};
        q.tryPop(out);
        q.tryPop(out);
        CHECK_INT_EQ(q.approximateDepth(), 3u);
        q.shutdown();
    }

    TEST_CASE(ShutdownClearsState) {
        AudioCommandQueue q;
        q.initialize(8);
        AudioCommand cmd{};
        cmd.type = AudioCommandType::StopAll;
        q.tryPush(cmd);
        q.shutdown();
        CHECK_INT_EQ(q.capacity(), 0u);
        CHECK(!q.isInitialized());
        AudioCommand out{};
        CHECK(!q.tryPop(out));
    }

    TEST_CASE(InitializeIsIdempotent) {
        AudioCommandQueue q;
        CHECK(q.initialize(8));
        CHECK(q.initialize(8)); // second call returns true, no resize
        CHECK_INT_EQ(q.capacity(), 8u);
        q.shutdown();
    }    
TEST_SUITE_END
