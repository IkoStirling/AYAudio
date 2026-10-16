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

Audio Editor (both hosts) has five tabs: instrument performance, single-resource
editing, engine mixing, recording and chart authoring. The piano appears only in performance. Its grip or
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
Complex/unsupported sidecars are read-only. MIDI and device-capture multi-track recording, resource-page rendered
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

Performance shortcuts: Left Shift adds one octave; Left Alt adds one semitone (C→C#, E→F).
Their offsets add when held together. A S D F G H J K are the base C..C white keys;
Q W E R T Y U I are the octave-above white keys; Z X C V B N M comma are the octave below. Temporary offsets add to fixed
transpose on note-on; held notes retain their original pitch until release.
Focus loss clears the temporary state. Right modifiers and text editing do not
trigger these shortcuts. The meter occupies 150 x 80 logical pixels in the
header; readings below its -60 dBFS range display -inf instead of decay noise.


### Recording desk and native waveform selection

The shared workbench now uses a continuous five-tab header: performance,
resources, mixer, recording and chart authoring. Selection tint is composited into the waveform
background before peaks, handles and cursor are drawn, so native GDI and GPU
hosts display the same visible waveform rather than an opaque blue overlay.

AudioRecorder enumerates OS capture/playback endpoints, including installed
virtual drivers (no driver installation). Owner-thread start/update/stop owns
device lifetimes; callbacks only use preallocated SPSC PCM rings. Capture,
pause/resume, optional input monitoring, Windows WASAPI output loopback, take
playback to a selected endpoint, peaks, dropped-frame diagnostics and PCM16 WAV
saving are implemented. Loopback cannot monitor back into its source. Default
48kHz stereo takes are limited to ten minutes; input buffer overflow is reported.

The recording tab defaults to `audio/recordings/Take_<timestamp>.wav` below the
current project's configured writable asset root. Standalone users select the
project's asset directory. No project: capture is allowed in memory, save waits
for a root. Stop then Save WAV; saved takes can open in Resources for range/fade
editing. Paths escaping the root and existing destinations are rejected.
Recording output selection applies to this desk's monitoring/playback/loopback;
the Host AudioEngine's output and mixer buses are independent. Endpoint choices
are session-local. Closing releases recording devices and discards unsaved PCM.


## Four-lane chart authoring (2026-10-05)

The fifth workbench tab, 谱面编辑, selects music, edits BPM/beat offset and
1..1/16-beat snapping, and shares the waveform viewport with a four-row D/F/J/K
timeline. Click empty space for a tap, drag for a hold, drag an existing note to
move it, right-click to delete, middle-drag to pan, and wheel to zoom. Numeric
lane/start/end editing and note undo/redo are available.

Save/Open use an Assets-relative `audio/charts/*.chart.json` path. Standalone
users choose an asset folder. In-project music is stored relative to that root;
external music retains an absolute reference. Save validates duration and same
lane overlaps and refuses a project switch, an unrelated existing file, or an
external edit. Original audio stays intact.

运行谱面 launches `AYAudio_AudioEditor.exe --play-chart <snapshot>` in an
independent window, including unsaved notes/settings. It has a selectable 0/0.5/2-second
preparation countdown (default 0.5), four horizontal lanes, per-note accuracy/error display, restart,
Esc pause/resume and automatic focus-loss pause. The preview owns its music voice;
the parent stops its own audition, instrument keys and stream before launching.
`AYEditorStage` also stages this executable/assets for the portable editor.

This is an authoring preview, not a recommended gameplay API. Timing follows
normalized clip playback frames with at most 50 ms interpolation; output/input latency
calibration, variable BPM, configurable key maps, scoring persistence and chart
cooking are deferred. Undo/redo covers notes and BPM/beat-offset calibration. Temporary preview
snapshots are separate from saved assets. See `design.md` for schema/validation
and `AudioChartTests` / `EditorAudioChartTests` for focused regressions.

## Performance clips and arrangement (2026-10-06)

The first tab is now 演奏与编排: a scrollable clip timeline/piano roll with a pinned piano strip.
Real-time takes retain raw timing/sustain and default to nearest-grid head/end snapping (minimum one cell); disabling end snap preserves physical duration. Step chords use selected
1/half/quarter/eighth-beat duration and restore the cursor with undo/redo. Direct track headers
replace track dropdown selection; drag the ruler/blank timeline or middle-drag to pan, wheel to zoom.
Clips support independent copy, cross-track move, non-destructive trim and repeats. Track mute,
solo and gains belong to the authoring document, not the shared engine mixer.
Immutable worker rendering places notes on integer sample frames and powers both owned-voice
preview and PCM16 WAV export; overlapping equal pitches have independent envelopes. Preparation
is cancellable. Arrangement/clip/range auditions keep playing when the window loses focus, including
preparation already in progress. Focus loss still releases live piano keys and stops/finalizes recording;
host replacement and teardown cancel preparation and release owned playback. Real-time input uses monotonic event-boundary timestamps, not UI dt; no calibrated
device-latency or hard-realtime guarantee. PerformanceRenderer is internal demo/editor support.
Save/open *.performance.json under project Assets; source baseline/path/project checks protect saves.
Export all or cursor..range-end with release tails, excluding metronome. Missing imported samples fail.
Limits: fixed BPM/4:4, 64 tracks, 4096 clips, 100k notes, 600s/256MiB output, 1e9 accumulated note frames.
See [authoring guide](../../AYDocs/AUDIO-AUTHORING.md) §8 and AudioPerformanceTests/EditorAudioPerformanceTests.

## Rhythm-assisted chart authoring (2026-10-06)

Song import asynchronously decodes, extracts waveform peaks, estimates fixed BPM/beat phase and
normalizes PCM. Reliable estimates apply automatically; silence/steady tones/low-confidence results
retain manual settings. Manual edits during preparation take precedence. BPM half/double, ±1ms beat
offset and setting the current song position as beat one are editable and undoable.
Waveform and chart share seconds/bar:beat grid, cursor, zoom/pan, live seeking and optional follow.
Generate rhythm draft replaces notes only on explicit action, with sparse/standard/dense real attack taps
and one reversible history entry. Original attack times are retained by default; optional snapping moves
only attacks already within 25ms of a beat cell. Density selects strong attacks, never fills empty beats.
Waveform yellow ticks expose detected attacks; half-beat grid fit reports ±35ms alignment.
Low/high-frequency attack evidence assigns left/right lane pairs, alternating within each pair. This is
coarse spectral placement, not instrument separation, pitch transcription or musical phrasing. This is an onset-based draft, not semantic musical transcription.
Preview uses a reusable decoded temporary PCM16 WAV, background preparation and asynchronous child decode.
Horizontal D/F/J/K lanes show heads, holds and recent grade/accuracy/early-late errors. Accuracy is
max(0,1−|error|/150ms)×100; holds take the lower endpoint accuracy, misses are 0. Total is the mean
of finished-note accuracies. Perfect/Good windows remain ±45/150ms; this is not latency calibration.
Limits: fixed tempo estimated in 60–200 BPM, 4/4 display, ≤600s/256MiB source and normalized PCM;
first 180s inform tempo, the full song informs draft onsets. Temporary caches live in OS Temp.
See AUDIO-AUTHORING §11 and AudioChartTests/EditorAudioChartTests.

Preview close dismisses the window before teardown and skips the final render frame. The HWND remains
alive until GPU shutdown; voices, jobs and devices still clean up normally. Small per-stage timings are
written locally to OS Temp/AYAudioChartPreview/Exit_<pid>.log for driver/worker diagnostics.

## Debug content extension (2026-10-15)

AudioEngine::debugSnapshot owns coherent listener, active/paused voice, bus gain/peak and output observations under the existing audio state mutex. Mixer bus meters sum spatial/voice/bus/duck/master-gained dry samples per bus before measuring amplitude; destructive interference is preserved. The fixed preallocated meter scratch adds no callback allocation. Peaks exclude reverb and are not RMS/LUFS. Reset/silent blocks clear meters. Diagnostics never seek or consume voices.

2026-10-16: rebuilt AYAudio_Tests and ran the full module suite: 174 selected, 172 passed, 2 explicit skips (opt-in WASAPI capture and absent optional MP3), 1379 assertions passed. Reset clears bus/output meters; paused or shut-down snapshots report zero bus peaks. No interactive editor acceptance is claimed. Raw report: out/audio-completion-20261016/baseline-native.json.
