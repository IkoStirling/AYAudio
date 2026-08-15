# AYAudio

Runtime presentation audio for AY Engine (mixing, spatialization, device output).

- **Design:** [design.md](design.md)
- **Backend:** miniaudio (not OpenAL, not SDL_audio)
- **Assets:** runtime mixes **PCM only**. Authoring uses common formats (WAV/MP3/OGG) via **AYResource** decode; cook to `.ayaudio` PCM for ship / optimize (design §2.3 lock).

## Status

v2.0b ship — miniaudio backend + streaming voices + ducking + Schroeder
reverb + doppler + custom attenuation curves + AYResource PCM bridge.

- **Next (v2.1):** ✅ Dev loose MP3/OGG + pack-time cook (design §2.3.1 A1–A6, 2026-08-13).
- **Gallery:** `AYAudio_Gallery` — standalone Win32 mixer / playback smoke UI.
- **Audio Editor:** `AYAudio_AudioEditor` — AYUI mixer desk (dual-host with AYEditor Tools).

## Gallery

```bat
d:\Projects\do_cmake.bat --build d:\Projects\out\build\x64-Debug --target AYAudio_Gallery
out\build\x64-Debug\AYRuntime\AYAudio\demo\AYAudio_Gallery.exe
```

Built-in sine one-shots (440 / 880 / loop 220), bus gains, spatial L/C/R,
ducking, reverb, timeScale, Pause/Stop, and **Open Stream…** for WAV/MP3/OGG
via `AudioSubSystem::playStreamingFile`.

## Audio Editor (AYUI)

Shared `AudioEditorSession` + `audio_editor.ui.json` (Layout Editor dual-host pattern).

```bat
d:\Projects\do_cmake.bat --build d:\Projects\out\build\x64-Debug --target AYAudio_AudioEditor
out\build\x64-Debug\AYRuntime\AYAudio\demo\AYAudio_AudioEditor.exe
```

In AYEditor: **Tools → Audio Editor…** (requires AudioSubSystem; skip with `-no-audio`).

## Tests

```bash
cmake --build build --target AYAudio_Tests
./build/AYRuntime/AYAudio/unittest/AYAudio_Tests
```
