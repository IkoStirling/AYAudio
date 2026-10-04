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

Audio Editor (both hosts) has three pages: instrument performance, single-resource
editing and engine mixing. The piano appears only in performance. Its grip or
middle button pans the key strip; the wheel changes key width around the cursor.
A W S E D F T G Y H U J K plays C..C; Space sustains. Signed semitone transpose
supports -24..+24, updates displayed/played pitches and releases held notes before
changing mappings. Numeric entry suppresses note-on. Focus loss releases notes
and modifier state. Performance presets edit gain/attack/release parameters.
`SampleInstrument::setBus` selects the bus for future notes (SFX by default).

Mixer rows are name | slider | editable percentage. Master/Music/SFX/UI/Voice
and reverb return address the current shared engine and are session values, not
automatically saved project/OS settings. A compact upper-right vertical L/R meter reads
`AudioEngine::outputLevels`: actual post-mix sample peaks before final clipping,
with dBFS display, decaying peak hold and overload indication. It is not RMS/LUFS.

The resource page decodes real PCM waveform peaks, previews source intervals,
and edits gain/fades with undo/redo. Save writes the existing schema-v2
`*.timeline.json` atomically; source PCM remains intact. A selected interval
becomes a clip at timeline 0 with sourceOffset; timeline preview is 0..clip length.
Complex/unsupported sidecars are read-only. MIDI, multi-track recording, rendered
audio export, arbitrary volume envelopes, saved preset libraries and a separate
editor preview bus remain disabled placeholders.

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

Native workbench attachment recalculates layout immediately, even when the host
already cached an empty mount panel. Tab changes also recalculate immediately;
no window resize or per-frame layout call is required. The EditorAudioTests
suite covers native/standalone load order, reattachment and tab switching.

Performance shortcuts: Left Shift temporarily adds two semitones; Left Alt
subtracts two, and holding both cancels. Z X C V B N M comma play the white
keys C..C one octave below the main row. Temporary offsets add to fixed
transpose on note-on; held notes retain their original pitch until release.
Focus loss clears the temporary state. Right modifiers and text editing do not
trigger these shortcuts. The meter occupies 150 x 80 logical pixels in the
header; readings below its -60 dBFS range display -inf instead of decay noise.
