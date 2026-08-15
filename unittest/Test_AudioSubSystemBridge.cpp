// Test_AudioSubSystemBridge.cpp
//
// Unit tests for AudioSubSystem::registerClipFromResource(IAudio*).
// Stands up a fake ayt::resource::IAudio (no on-disk asset needed) and
// verifies the bridge correctly maps the asset fields into AudioClipDesc,
// registers a clip, and rejects unsupported bit depths / channel counts.

#include <AYAudio.h>
#include <AYAudioSubSystem.h>

#include <IAYAudio.h>   // ayt::resource::IAudio
#include <AYMath/MathDefs.h> // brings UInt8/UInt32/UInt64/Float32 into global ns

#include "AYTest.h"

#include <cstring>
#include <vector>

using namespace ayt::audio;

namespace {

// Minimal hand-rolled IAudio stub. Only the getters used by the bridge
// need to return real data; the rest of the IAudio API is left as a
// no-op for the tests.
class StubAudio final : public ayt::resource::IAudio {
public:
    StubAudio(uint32_t rate, uint32_t channels, uint32_t bps,
              std::vector<uint8_t> pcm)
        : _rate(rate), _channels(channels), _bps(bps), _pcm(std::move(pcm)) {}

    const char* getName() const override { return "stub"; }
    unsigned int getSampleRate() const override { return _rate; }
    unsigned int getChannels() const override { return _channels; }
    unsigned int getBitsPerSample() const override { return _bps; }
    unsigned long long getSampleCount() const override {
        // sample count = total samples (frames * channels flattened).
        const uint32_t bytesPerFrame = _bps / 8;
        if (bytesPerFrame == 0) return 0;
        return _pcm.size() / bytesPerFrame;
    }
    const unsigned char* getData() const override { return _pcm.data(); }
    float getDuration() const override {
        if (_rate == 0 || _channels == 0) return 0.0f;
        const uint64_t frames = getSampleCount() / _channels;
        return static_cast<float>(frames) / static_cast<float>(_rate);
    }
    unsigned int getDataSize() const override {
        return static_cast<unsigned int>(_pcm.size());
    }

    // IResource — stub satisfies the interface but we never use these.
    bool load(const std::string& /*path*/) override { return true; }
    bool unload() override { return true; }
    size_t sizeInBytes() const override { return sizeof(*this) + _pcm.size(); }

private:
    uint32_t _rate;
    uint32_t _channels;
    uint32_t _bps;
    std::vector<uint8_t> _pcm;
};

AudioSettings defaultSettings() {
    AudioSettings s{};
    s.backend = AudioBackendKind::Null;
    s.sampleRate = 48000;
    s.channels = 2;
    s.commandQueueCapacity = 32;
    s.maxVoices = 8;
    s.maxClips = 8;
    return s;
}

} // namespace

TEST_SUITE(AudioSubSystemBridgeTests)

    TEST_CASE(RegisterFrom16BitPcm) {
        AudioSubSystem sub;
        CHECK(sub.initialize());

        // 16-bit mono, 1 frame, 2 bytes of PCM.
        StubAudio audio(48000, 1, 16, std::vector<uint8_t>{ 0x00, 0x40 });
        const AudioClipId id = sub.registerClipFromResource(&audio);
        CHECK(id != ayt::audio::InvalidClipId);
        CHECK(sub.engine()->clipRegistry().size() == 1);

        sub.shutdown();
    }

    TEST_CASE(RegisterFrom32BitFloatPcm) {
        AudioSubSystem sub;
        CHECK(sub.initialize());

        // 32-bit float stereo, 2 frames = 16 bytes.
        const std::vector<uint8_t> pcm(16, 0);
        StubAudio audio(44100, 2, 32, pcm);
        const AudioClipId id = sub.registerClipFromResource(&audio);
        CHECK(id != ayt::audio::InvalidClipId);
        CHECK(sub.engine()->clipRegistry().size() == 1);

        sub.shutdown();
    }

    TEST_CASE(RegisterFromStereoS16HasCorrectMapping) {
        AudioSubSystem sub;
        CHECK(sub.initialize());

        StubAudio audio(22050, 2, 16, std::vector<uint8_t>(8, 0));
        const AudioClipId id = sub.registerClipFromResource(&audio);
        CHECK(id != ayt::audio::InvalidClipId);

        // Reach into the registry: sample-rate policy resamples 22050 → 48k.
        const auto* slot = sub.engine()->clipRegistry().find(id);
        CHECK(slot != nullptr);
        CHECK(slot->desc.sampleRate == 48000u);
        CHECK(slot->desc.channels == 2);
        CHECK(slot->desc.bitsPerSample == 16);
        CHECK(slot->desc.format == ayt::audio::AudioSampleFormat::S16);
        // 2 source frames @ 22050 → ~4 frames @ 48000.
        CHECK(slot->desc.frameCount >= 4u);
        CHECK(slot->desc.frameCount <= 5u);

        sub.shutdown();
    }

    TEST_CASE(RegisterRejectsUnsupportedBitDepth) {
        AudioSubSystem sub;
        CHECK(sub.initialize());

        // 8-bit PCM and 24-bit PCM are not implemented in AudioEngine's
        // voice source dispatch, so the bridge must refuse them.
        StubAudio audio8(48000, 1, 8, std::vector<uint8_t>(4, 0));
        StubAudio audio24(48000, 1, 24, std::vector<uint8_t>(12, 0));
        CHECK(sub.registerClipFromResource(&audio8) == ayt::audio::InvalidClipId);
        CHECK(sub.registerClipFromResource(&audio24) == ayt::audio::InvalidClipId);
        CHECK(sub.engine()->clipRegistry().size() == 0);

        sub.shutdown();
    }

    TEST_CASE(RegisterRejectsInvalidChannelCount) {
        AudioSubSystem sub;
        CHECK(sub.initialize());

        // 3-channel audio isn't supported by AudioMixer.
        StubAudio audio(48000, 3, 16, std::vector<uint8_t>(12, 0));
        CHECK(sub.registerClipFromResource(&audio) == ayt::audio::InvalidClipId);

        sub.shutdown();
    }

    TEST_CASE(RegisterRejectsZeroSampleCount) {
        AudioSubSystem sub;
        CHECK(sub.initialize());

        // Empty PCM buffer → sample count == 0 → rejected.
        StubAudio audio(48000, 1, 16, std::vector<uint8_t>{});
        CHECK(sub.registerClipFromResource(&audio) == ayt::audio::InvalidClipId);

        sub.shutdown();
    }

    TEST_CASE(RegisterRejectsNullPointer) {
        AudioSubSystem sub;
        CHECK(sub.initialize());

        CHECK(sub.registerClipFromResource(nullptr) == ayt::audio::InvalidClipId);

        sub.shutdown();
    }

    TEST_CASE(RegisterRejectedBeforeInitialize) {
        // Construct a sub but never call initialize() — the bridge should
        // refuse rather than crash.
        AudioSubSystem sub;
        StubAudio audio(48000, 1, 16, std::vector<uint8_t>(2, 0));
        CHECK(sub.registerClipFromResource(&audio) == ayt::audio::InvalidClipId);
    }

TEST_SUITE_END