# AYAudio

Runtime presentation audio for AY Engine (mixing, spatialization, device output).

- **Design:** [design.md](design.md)
- **Backend:** miniaudio (not OpenAL, not SDL_audio)
- **Assets:** decoded PCM from `AYResource` (`.ayaudio` cook path today; see design §3)

## Status

v2.0b ship — miniaudio backend + streaming voices + ducking + Schroeder
reverb + doppler + custom attenuation curves + AYResource bridge.

## Tests

```bash
cmake --build build --target AYAudio_Tests
./build/AYRuntime/AYAudio/unittest/AYAudio_Tests
```
