# AYAudio

Runtime presentation audio for AY Engine (mixing, spatialization, device output).

- **Design:** [design.md](design.md)
- **Backend:** miniaudio (not OpenAL, not SDL_audio)
- **Assets:** decoded PCM from `AYResource` (`.ayaudio` cook path today; see design §3)

## Status

Planning — no implementation in tree yet.

## Tests (when implemented)

```bash
cmake --build build --target AYAudio_unittest
./build/unittest/AYAudio_unittest
```
