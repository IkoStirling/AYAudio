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

Default Client / Editor hosts install `AudioSubSystem` through
`AudioRuntimeModule` (`AYAudio.Runtime`). The module accepts a backend factory,
so a host may select Miniaudio, Null, or a custom backend without bypassing the
module graph. The default `-no-audio` option omits the module entirely; direct
subsystem construction remains available to demos and tests.

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

## Instrument playback and audio authoring (2026-10-04)

`SampleInstrument` is the game-thread sampler over the Host-owned `AudioEngine`.
Preload PCM through AYResource, register clips, add `InstrumentRegion` mappings,
then call `noteOn` / `noteOff` / `setSustain`. Velocity is 0..1, MIDI note numbers
are 0..127. Region pitch ratios outside 0.5..4 are rejected; choose nearby root
samples. Polyphony steals released notes before oldest held notes. Region clips
are borrowed and must outlive the instrument. Destroy/detach it before destroying
its engine (or explicitly `abandon` when the host engine has already disappeared).

VoiceHandle is an opaque 64-bit identity; use `voiceState` / `voiceStateAt` copies,
never `voiceAt(handle-1)`. Stop/reuse and reinitialization invalidate old voices.
`releaseVoice` uses an audio-frame fade, `setVoicePaused` pauses only one voice,
and `setVoicePlaybackRange` selects a clip's exclusive source-frame interval.
PCM fractional pitch position persists across buffers. Clip memory is copied.
Public state mutation and callback rendering are serialized; the callback uses
a try-lock and emits silence on contention rather than waiting. Preload outside
live performance; this is not a hard realtime/no-dropout guarantee.

Audio Editor (both hosts): A W S E D F T G Y H U J K plays C..C one octave above;
Space is sustain, mouse presses play immediately, and focus loss releases notes.
Octave buttons cover C1..C6 starts. Preview volume/velocity, piano/sine switching,
and Load Sample are available. Imported samples use the displayed octave's C as
root and cover root-12..root+24 subject to pitch limits. Preview pause/stop affect
only session voices; bus/effect controls deliberately affect the global mix.

Default C1..C7 piano samples are Salamander Grand Piano by Alexander Holm,
CC BY 3.0, redistributed as a Tonejs/audio MP3 subset. Attribution, upstream
README, license and SHA-256 manifest travel with the samples. Missing assets
fall back to generated tones. Download source lives in AssetRepo; run the root
`scripts/sync_audio_assets.ps1` to verify and stage selected assets. Standalone
POST_BUILD and AYEditorStage install copy assets; there is no runtime AssetRepo
dependency. See `AYDocs/AUDIO-AUTHORING.md` and SDK `common.audio.instrument`.

Regression: `AYAudio_Tests --suite SampleInstrumentTests` plus the full audio suite.

Full-file sample loading uses AYResource AudioLoader. PCM frame count is derived
from validated byte length at the AYAudio bridge, avoiding stereo truncation
when decoder metadata counts per-channel frames. Audio documents use that same
PCM layout to derive source duration.
