# AYAudio

Runtime presentation audio for AY Engine (mixing, spatialization, device output).

- **Design:** [design.md](design.md)
- **Backend:** miniaudio (not OpenAL, not SDL_audio)
- **Assets:** runtime mixes **PCM only**. Authoring uses common formats (WAV/MP3/OGG) via **AYResource** decode; cook to `.ayaudio` PCM for ship / optimize (design §2.3 lock).

## Status

v2.0b ship — miniaudio backend + streaming voices + ducking + Schroeder
reverb + doppler + custom attenuation curves + AYResource PCM bridge.

- **Next (v2.1):** ✅ Dev loose MP3/OGG + pack-time cook (design §2.3.1 A1–A6, 2026-08-13).

## Tests

```bash
cmake --build build --target AYAudio_Tests
./build/AYRuntime/AYAudio/unittest/AYAudio_Tests
```
