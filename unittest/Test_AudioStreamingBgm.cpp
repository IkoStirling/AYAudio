#include <AYAudio.h>
#include <AYAudioSubSystem.h>

#include <AYIO/File.h>
#include <AYIO/Path.h>

#include "AYTest.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace ayt::audio;

namespace {

#pragma pack(push, 1)
struct WavHdr {
    char riff[4];
    uint32_t size;
    char wave[4];
    char fmt[4];
    uint32_t fmtSize;
    uint16_t format;
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bits;
    char data[4];
    uint32_t dataSize;
};
#pragma pack(pop)

std::string writeTempMonoWav(uint32_t rate, uint32_t frames) {
    std::vector<int16_t> pcm(frames, 8000);
    WavHdr h{};
    std::memcpy(h.riff, "RIFF", 4);
    std::memcpy(h.wave, "WAVE", 4);
    std::memcpy(h.fmt, "fmt ", 4);
    std::memcpy(h.data, "data", 4);
    h.fmtSize = 16;
    h.format = 1;
    h.channels = 1;
    h.sampleRate = rate;
    h.bits = 16;
    h.blockAlign = 2;
    h.byteRate = rate * 2;
    h.dataSize = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    h.size = 36 + h.dataSize;

    const std::string path = ayt::test::testTmpPath("stream_bgm_test.wav");
    ayt::io::File f(path, ayt::io::File::Mode::BinaryWrite);
    if (!f.isOpen()) {
        return {};
    }
    f.write(&h, sizeof(h));
    f.write(pcm.data(), h.dataSize);
    return path;
}

} // namespace

TEST_SUITE(AudioStreamingBgmTests)

    TEST_CASE(PlayStreamingFilePumpsAndRenders) {
        AudioSubSystem sub;
        CHECK(sub.initialize());
        CHECK(sub.engine() != nullptr);

        const std::string path = writeTempMonoWav(22050, 4096);
        CHECK(!path.empty());

        StreamingPlayback play = sub.playStreamingFile(path, AudioBus::Music, false, 1.0f);
        CHECK(play.stream != InvalidStreamId);
        CHECK(play.voice != InvalidVoice);

        // Pump a few frames; decoder should fill the ring.
        for (int i = 0; i < 4; ++i) {
            sub.update(1.0f / 60.0f);
        }

        float out[256]{};
        sub.engine()->render(out, 128);
        float peak = 0.0f;
        for (float s : out) {
            const float a = s < 0.0f ? -s : s;
            if (a > peak) peak = a;
        }
        CHECK(peak > 0.01f);

        sub.stopStreaming(play);
        sub.shutdown();
    }

    // Regression: ambient MP3 beds must produce non-silent stream pump + mix.
    // Path is optional (dev asset tree); skip when missing.
    TEST_CASE(EveningWandersMp3StreamAudible) {
        const char* kPath =
            "D:/Projects/AliyatRenderer/assets/core/audios/ambient/"
            "Evening_wanders.mp3";
        ayt::io::File probe(kPath, ayt::io::File::Mode::BinaryRead);
        if (!probe.isOpen()) {
            return; // asset not present in this checkout
        }
        probe.close();

        AudioSubSystem sub;
        CHECK(sub.initialize());

        StreamingPlayback play =
            sub.playStreamingFile(kPath, AudioBus::Music, false, 1.0f);
        CHECK(play.stream != InvalidStreamId);
        CHECK(play.voice != InvalidVoice);

        float maxPeak = 0.0f;
        for (int i = 0; i < 120; ++i) {
            sub.update(1.0f / 60.0f);
            float out[1024]{};
            sub.engine()->render(out, 512);
            for (float s : out) {
                const float a = s < 0.0f ? -s : s;
                if (a > maxPeak) {
                    maxPeak = a;
                }
            }
        }
        // Ambient beds can be quiet, but must not be digital silence.
        CHECK(maxPeak > 0.001f);

        sub.stopStreaming(play);
        sub.shutdown();
    }

TEST_SUITE_END
