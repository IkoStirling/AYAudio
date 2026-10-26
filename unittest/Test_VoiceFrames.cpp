#include <AYTest.h>
#include <AYAudio/VoiceFrames.h>
#include <AYAudio/VoiceCapture.h>
#include <AYAudio/VoicePlaybackRoute.h>
#include <AYAudio/VoiceRoomRouter.h>
#include <AYAudio/VoicePacketIngress.h>
#include <array>
#include <limits>
#include <memory>

using namespace ayt::audio;
TEST_SUITE(VoiceFramesTests)
TEST_CASE(pcm16_wire_roundtrip_and_malformed_packet_are_atomic) {
    std::vector<float> samples(960);
    samples[0] = -1.f;
    samples[1] = 0.5f;
    samples[2] = std::numeric_limits<float>::quiet_NaN();
    samples[3] = 2.f;
    std::vector<std::uint8_t> encoded{42};
    CHECK(VoicePcmWire::encode(5, 7, 48'000, 1, samples.data(), 960, encoded));
    CHECK_INT_EQ(encoded.size(), VoicePcmWire::HeaderBytes + 1920);
    VoicePcmFrame decoded;
    CHECK(VoicePcmWire::decode(encoded.data(), encoded.size(), decoded));
    CHECK_INT_EQ(decoded.speakerId, 5);
    CHECK_INT_EQ(decoded.sequence, 7);
    CHECK_INT_EQ(decoded.sampleRate, 48'000);
    CHECK_INT_EQ(decoded.samples[0], -32767);
    CHECK_INT_EQ(decoded.samples[1], 16384);
    CHECK_INT_EQ(decoded.samples[2], 0);
    CHECK_INT_EQ(decoded.samples[3], 32767);
    encoded[4] = 2;
    CHECK_FALSE(VoicePcmWire::decode(encoded.data(), encoded.size(), decoded));
    CHECK_INT_EQ(decoded.sequence, 7);
    CHECK_FALSE(VoicePcmWire::encode(5, 8, 48'000, 1, samples.data(), 959, encoded));
    CHECK_INT_EQ(encoded[4], 2);
}
TEST_CASE(reordering_duplicates_and_single_loss_concealment) {
    VoiceJitterQueue queue(9, 2);
    auto frame = [](std::uint32_t seq) {
        return VoicePcmFrame{9, seq, 8'000, 1, std::vector<std::int16_t>(160, 11)};
    };
    CHECK(queue.push(frame(11)));
    CHECK_INT_EQ(queue.pop().status, VoicePopStatus::Buffering);
    CHECK(queue.push(frame(10)));
    CHECK_FALSE(queue.push(frame(10)));
    CHECK_INT_EQ(queue.pop().frame.sequence, 10);
    CHECK_INT_EQ(queue.pop().frame.sequence, 11);
    CHECK(queue.push(frame(13)));
    const auto loss = queue.pop();
    CHECK_INT_EQ(loss.status, VoicePopStatus::Concealed);
    CHECK_INT_EQ(loss.frame.sequence, 12);
    CHECK_INT_EQ(loss.frame.samples.size(), 160);
    CHECK_INT_EQ(loss.frame.samples[0], 0);
    CHECK_INT_EQ(queue.pop().frame.sequence, 13);
}
TEST_CASE(mute_format_and_speaker_boundaries) {
    VoiceJitterQueue queue(3, 1);
    VoicePcmFrame one{3, 1, 16'000, 1, std::vector<std::int16_t>(320, 5)};
    CHECK(queue.push(one));
    auto wrongSpeaker = one;
    wrongSpeaker.speakerId = 4;
    CHECK_FALSE(queue.push(wrongSpeaker));
    auto wrongFormat = one;
    wrongFormat.sequence = 2;
    wrongFormat.sampleRate = 48'000;
    CHECK_FALSE(queue.push(wrongFormat));
    queue.setMuted(true);
    CHECK_FALSE(queue.push(one));
    CHECK_INT_EQ(queue.buffered(), 0);
    CHECK_INT_EQ(queue.pop().status, VoicePopStatus::Buffering);
    queue.setMuted(false);
    CHECK(queue.push(one));
    CHECK_INT_EQ(queue.pop().status, VoicePopStatus::Frame);
}
TEST_CASE(sequence_wrap_and_starvation_reprime) {
    VoiceJitterQueue queue(1, 1);
    auto frame = [](std::uint32_t seq) {
        return VoicePcmFrame{1, seq, 8'000, 1, std::vector<std::int16_t>(160, 1)};
    };
    CHECK(queue.push(frame(std::numeric_limits<std::uint32_t>::max())));
    CHECK_INT_EQ(queue.pop().frame.sequence, std::numeric_limits<std::uint32_t>::max());
    CHECK(queue.push(frame(0)));
    CHECK_INT_EQ(queue.pop().frame.sequence, 0);
    CHECK_INT_EQ(queue.pop().status, VoicePopStatus::Concealed);
    CHECK_INT_EQ(queue.pop().status, VoicePopStatus::Concealed);
    CHECK_INT_EQ(queue.pop().status, VoicePopStatus::Buffering);
    CHECK(queue.push(frame(100)));
    CHECK_INT_EQ(queue.pop().frame.sequence, 100);
}
TEST_CASE(capture_callback_drains_exact_live_frames_and_mute_discards_backlog) {
    VoiceCapture capture;
    CHECK(VoiceCaptureTestAccess::beginBuffered(capture, {{}, 8'000, 1}));
    std::vector<float> input(320, 0.25f), out{5.f};
    VoiceCaptureTestAccess::receive(capture, input.data(), 159);
    CHECK_FALSE(capture.pollFrame(out));
    CHECK_INT_EQ(out.size(), 1);
    VoiceCaptureTestAccess::receive(capture, input.data(), 161);
    CHECK(capture.pollFrame(out));
    CHECK_INT_EQ(out.size(), 160);
    CHECK(out[0] == 0.25f);
    capture.setMuted(true);
    VoiceCaptureTestAccess::receive(capture, input.data(), 160);
    CHECK_FALSE(capture.pollFrame(out));
    capture.setMuted(false);
    CHECK_FALSE(capture.pollFrame(out));
    VoiceCaptureTestAccess::receive(capture, input.data(), 160);
    CHECK(capture.pollFrame(out));
    CHECK_INT_EQ(capture.droppedFrames(), 0);
    capture.stop();
    CHECK_FALSE(capture.running());
}
TEST_CASE(remote_speaker_reaches_voice_bus_and_mute_reopens_fresh_stream) {
    AudioEngine engine;
    AudioSettings settings{};
    settings.backend = AudioBackendKind::Null;
    settings.sampleRate = 48'000;
    settings.channels = 2;
    CHECK(engine.initialize(settings));
    VoicePlaybackRoute route(5, 1);
    CHECK(route.open(engine));
    CHECK(route.voice() != InvalidVoice);
    CHECK(route.submit({5, 0, 48'000, 1, std::vector<std::int16_t>(960, 16384)}));
    CHECK_INT_EQ(route.pump(engine), VoicePopStatus::Frame);
    engine.submitFrame(0.f);
    float output[64]{};
    engine.render(output, 32);
    CHECK(output[0] > 0.1f);
    CHECK(route.setMuted(engine, true));
    CHECK(route.voice() == InvalidVoice);
    CHECK_FALSE(route.submit({5, 1, 48'000, 1, std::vector<std::int16_t>(960, 16384)}));
    CHECK(route.setMuted(engine, false));
    CHECK(route.submit({5, 2, 48'000, 1, std::vector<std::int16_t>(960, 16384)}));
    CHECK_INT_EQ(route.pump(engine), VoicePopStatus::Frame);
    route.close(engine);
    engine.shutdown();
}
TEST_CASE(captured_block_survives_wire_jitter_and_null_backend_render) {
    VoiceCapture capture;
    CHECK(VoiceCaptureTestAccess::beginBuffered(capture, {{}, 48'000, 1}));
    std::vector<float> microphone(960, 0.5f), block;
    VoiceCaptureTestAccess::receive(capture, microphone.data(), 960);
    CHECK(capture.pollFrame(block));
    std::vector<std::uint8_t> wire;
    CHECK(VoicePcmWire::encode(99, 3, 48'000, 1, block.data(), 960, wire));
    VoicePcmFrame decoded;
    CHECK(VoicePcmWire::decode(wire.data(), wire.size(), decoded));
    AudioEngine engine;
    AudioSettings settings{};
    settings.backend = AudioBackendKind::Null;
    settings.sampleRate = 48'000;
    CHECK(engine.initialize(settings));
    VoicePlaybackRoute route(99, 1);
    CHECK(route.open(engine));
    CHECK(route.submit(std::move(decoded)));
    CHECK_INT_EQ(route.pump(engine), VoicePopStatus::Frame);
    engine.submitFrame(0.f);
    float output[64]{};
    engine.render(output, 32);
    CHECK(output[0] > 0.1f);
    route.close(engine);
    engine.shutdown();
    capture.stop();
}
TEST_CASE(configured_room_stream_budget_admits_twelve_independent_speakers) {
    AudioEngine engine;
    AudioSettings settings{};
    settings.backend = AudioBackendKind::Null;
    settings.maxVoices = 16;
    settings.maxStreams = 12;
    CHECK(engine.initialize(settings));
    std::vector<std::unique_ptr<VoicePlaybackRoute>> speakers;
    for (std::uint64_t id = 1; id <= 12; ++id) {
        auto route = std::make_unique<VoicePlaybackRoute>(id, 1);
        CHECK(route->open(engine));
        CHECK(route->submit({id, 0, 48'000, 1,
            std::vector<std::int16_t>(960, 2048)}));
        CHECK_INT_EQ(route->pump(engine), VoicePopStatus::Frame);
        speakers.push_back(std::move(route));
    }
    VoicePlaybackRoute extra(13, 1);
    CHECK_FALSE(extra.open(engine));
    engine.submitFrame(0.f);
    float output[64]{};
    engine.render(output, 32);
    CHECK(output[0] > 0.0f);
    CHECK(speakers[0]->setMuted(engine, true));
    CHECK(extra.open(engine));
    extra.close(engine);
    for (auto& speaker : speakers) speaker->close(engine);
    engine.shutdown();

    AudioEngine invalid;
    settings.maxStreams = 65;
    CHECK_FALSE(invalid.initialize(settings));
}
TEST_CASE(room_router_virtualizes_distant_speakers_without_old_audio_backlog) {
    AudioEngine engine;
    AudioSettings settings{};
    settings.backend = AudioBackendKind::Null;
    settings.maxStreams = 2;
    settings.maxVoices = 4;
    CHECK(engine.initialize(settings));
    VoiceRoomRouter room(2, 1);
    const auto candidate = [](std::uint64_t id, std::int32_t priority,
                              float distance, bool muted = false) {
        VoiceRoomCandidate value;
        value.speakerId = id; value.priority = priority;
        value.distanceMeters = distance; value.muted = muted;
        return value;
    };
    const std::array first{candidate(1, 0, 1), candidate(2, 0, 2),
                           candidate(3, 1, 100), candidate(4, 10, 0, true)};
    const auto entered = room.update(engine, first);
    CHECK_INT_EQ(entered.status, VoiceRoomUpdateStatus::Ok);
    CHECK_INT_EQ(entered.active, 2);
    CHECK(room.audible(1) && room.audible(3));
    CHECK_FALSE(room.audible(2) || room.audible(4));
    CHECK_FALSE(room.submit({2, 0, 48'000, 1,
        std::vector<std::int16_t>(960, 2048)}));
    CHECK(room.submit({1, 0, 48'000, 1,
        std::vector<std::int16_t>(960, 2048)}));
    room.pump(engine);
    const std::array changed{candidate(1, 0, 1), candidate(2, 2, 2),
                             candidate(3, 1, 100)};
    const auto switched = room.update(engine, changed);
    CHECK_INT_EQ(switched.status, VoiceRoomUpdateStatus::Ok);
    CHECK(room.audible(2) && room.audible(3));
    CHECK_FALSE(room.audible(1));
    CHECK_FALSE(room.submit({1, 1, 48'000, 1,
        std::vector<std::int16_t>(960, 2048)}));
    CHECK(room.submit({2, 0, 48'000, 1,
        std::vector<std::int16_t>(960, 2048)}));
    const std::array invalid{candidate(2, 0, 1), candidate(2, 1, 2)};
    CHECK_INT_EQ(room.update(engine, invalid).status,
        VoiceRoomUpdateStatus::Invalid);
    CHECK(room.audible(2) && room.audible(3));
    room.closeAll(engine);
    CHECK_INT_EQ(room.activeCount(), 0);
    engine.shutdown();
}
TEST_CASE(authenticated_voice_ingress_limits_packets_and_discards_inactive_routes) {
    AudioEngine engine;
    AudioSettings settings{};
    settings.backend = AudioBackendKind::Null;
    settings.maxStreams = 2;
    CHECK(engine.initialize(settings));
    VoiceRoomRouter room(1, 1);
    VoiceRoomCandidate speaker;
    speaker.speakerId = 7;
    CHECK_INT_EQ(room.update(engine, std::span<const VoiceRoomCandidate>(&speaker, 1)).active, 1);
    VoicePacketIngress ingress(2, 50);
    std::vector<float> samples(960, 0.25f);
    std::vector<std::uint8_t> wire, wrong;
    CHECK(VoicePcmWire::encode(7, 0, 48'000, 1, samples.data(), 960, wire));
    CHECK(VoicePcmWire::encode(8, 0, 48'000, 1, samples.data(), 960, wrong));
    CHECK_INT_EQ(ingress.accept(room, 7, wrong, 100),
                 VoiceIngressStatus::IdentityMismatch);
    CHECK_INT_EQ(ingress.accept(room, 7, wire, 100),
                 VoiceIngressStatus::Accepted);
    CHECK_INT_EQ(ingress.accept(room, 7, wire, 100),
                 VoiceIngressStatus::RateLimited);
    CHECK_INT_EQ(ingress.accept(room, 7, wire, 99),
                 VoiceIngressStatus::Invalid);
    CHECK(VoicePcmWire::encode(7, 1, 48'000, 1, samples.data(), 960, wire));
    CHECK_INT_EQ(ingress.accept(room, 7, wire, 120),
                 VoiceIngressStatus::Accepted);
    CHECK_INT_EQ(ingress.trackedSpeakers(), 1);
    room.pump(engine);
    engine.submitFrame(0.f);
    float output[64]{};
    engine.render(output, 32);
    CHECK(output[0] > 0.01f);
    auto malformed = wire;
    malformed[4] = 2;
    CHECK_INT_EQ(ingress.accept(room, 7, malformed, 140),
                 VoiceIngressStatus::Invalid);
    CHECK_INT_EQ(ingress.accept(room, 7, wire, 140),
                 VoiceIngressStatus::RateLimited);
    speaker.muted = true;
    CHECK_INT_EQ(room.update(engine, std::span<const VoiceRoomCandidate>(&speaker, 1)).active, 0);
    CHECK_INT_EQ(ingress.accept(room, 7, wire, 160),
                 VoiceIngressStatus::Inaudible);
    CHECK_INT_EQ(ingress.trackedSpeakers(), 0);
    room.closeAll(engine);
    engine.shutdown();
}
TEST_SUITE_END
