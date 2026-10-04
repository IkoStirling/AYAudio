# AYAudio — Design (v1.0)

**Status:** Living document — runtime audio module (not asset import)  
**Last revised:** 2026-07-11  
**Owner:** Engine / presentation systems

---

## 0. Reading guide

| Audience | Read |
|----------|------|
| All engine engineers | §1, §2 (decisions), §4 (architecture) |
| Implementers (v0) | §5–§7, §11 (phases) |
| Content / resource owners | §3 (format TBD), link to `AYResource/design.md` |
| ECS / gameplay | §8 (integration) |
| Other AI agents | §11 v0 scope + §12 (directory); **do not** touch Editor shutdown or Renderer composite |

Related docs:

- [`ENGINE-DETERMINISM-ARCHITECTURE.md`](../../ENGINE-DETERMINISM-ARCHITECTURE.md) — audio is **presentation-only**
- [`AYApplication/design.md`](../AYApplication/design.md) — `AudioSubSystem` registration, `-no-audio`, `[Audio]` config
- [`AYEntity/design.md`](../AYEntity/design.md) — system priority **600+** reserved for Audio
- [`AYEventSystem/design.md`](../AYEventSystem/design.md) — `AudioPlayEvent` placeholder
- [`AYResource/design.md`](../AYResource/design.md) — `.ayaudio` L1 format (separate from this module)
- [`AYDevice/design.md`](../AYDevice/design.md) — window/input (not audio output)
- [`AYRendering-Architecture-Roadmap.md`](../../AYRendering-Architecture-Roadmap.md) — render vs UI vs 3D passes

Legacy reference (out of tree): **AliyatRenderer** used **OpenAL** — **not ported**. Concepts only.

---

## 1. Mission and non-goals

### 1.1 Mission

`AYAudio` is the **runtime presentation audio engine**: device output, mixing, spatialization, and playback control. It consumes **decoded PCM** (or streams) and drives speakers on the **client** build.

### 1.2 Non-goals

| AYAudio does NOT | Owner instead |
|------------------|---------------|
| Import WAV/MP3/OGG from disk (policy TBD, see §3) | `AYResource` converters **or** optional runtime decoder in AYAudio |
| Own `.ayaudio` file format versioning | `AYResource` |
| Participate in lockstep / deterministic simulation | `ENGINE-DETERMINISM-ARCHITECTURE.md` |
| Run on **Server** build | `AYApplication` — Client-only subsystem |
| Replace **AYGraph AudioGraph** (node-based authoring) | `AYGraph` — AudioGraph v0 control cues on buses (see `AYGraph/design.md`) |
| Use **OpenAL** or **SDL_audio** as the primary backend | This design: **miniaudio** (§2) |

---

## 2. Technology decisions (locked / TBD)

### 2.1 Output backend: **miniaudio** ✅ (locked)

| Option | Verdict | Reason |
|--------|---------|--------|
| **OpenAL** (AliyatRenderer legacy) | ❌ Rejected | Aging API, rigid 3D model, deployment friction |
| **SDL2 audio** (`SDL_OpenAudioDevice`) | ❌ Not primary | Couples audio to optional `AY_DEVICE_USE_SDL2`; mixing/spatial control limited |
| **miniaudio** | ✅ **Chosen** | Single-file friendly, cross-platform, full control of mix graph in engine code |
| Platform-only (XAudio2 / WASAPI direct) | ⏸ Optional later | Possible `IAudioBackend` impl behind same interface |

**Implementation note:** Vendor `miniaudio` under `AYAudio/thirdparty/` (or shared engine thirdparty). Engine implements **mixer + spatializer**; miniaudio provides **device + callback**.

### 2.2 Relationship to AYDevice / SDL2

| Module | Role |
|--------|------|
| **AYDevice** | Window + input. Windows **default: Win32**; **SDL2 optional** via `-DAY_DEVICE_USE_SDL2=ON` (window/events only). |
| **AYAudio** | **Independent** initialization. Does **not** require SDL2. Does **not** call `SDL_Init(SDL_INIT_AUDIO)`. |

Rationale: Editor already uses Win32 host + bgfx; audio must work when SDL2 is off. Avoid init-order coupling (`SDL_Quit` vs device teardown).

### 2.3 Resource / decode path: **LOCKED** (2026-08-13)

**Policy (owner decision):** Authoring and day-to-day iteration use **common compressed / container formats** (WAV / MP3 / OGG first; FLAC optional later). **Cook to PCM (`.ayaudio`) only when packing for ship or when a clip needs load/CPU optimization.** `AYAudio` itself never parses file formats — it only consumes PCM / stream PCM.

| Layer | Owns | Notes |
|-------|------|-------|
| **AYResource** | Decode + cook | Dev: load loose `.wav`/`.mp3`/`.ogg` → PCM `IAudio`. Ship/opt: `AudioConverter` → `.ayaudio` (PCM L1). |
| **AYAudio** | Mix / play | `registerClip` / `playStream` / buses only. No MP3/OGG includes in public headers. |

Former strategy table (kept for history):

| Strategy | Role under this lock |
|----------|----------------------|
| **A. Cook-only `.ayaudio`** | **Ship / optimized** path (required for Release packaging) |
| **B. Dev loose formats** | **Default in Debug / Editor** — drop file, decode on load |
| **C. Runtime decode inside AYAudio** | **Rejected** as primary — decode stays in AYResource (keeps mixer free of codecs) |

**Libraries (decode in AYResource, not AYAudio):**
- WAV: existing PCM parser (extend to common WAVE variants as needed)
- MP3: `minimp3` (or equivalent single-header, license-clean)
- OGG Vorbis: `stb_vorbis` (or equivalent)
- Prefer decode on **game / loader thread** (or cook-time worker); never on the miniaudio callback thread

**Ship rules:**
- Release / packaged builds: **`.ayaudio` only** (or pak entries thereof). Loose MP3/OGG loaders compile out or hard-fail.
- Debug / Editor: loose formats allowed; optional background cook to `.ayaudio` for parity testing.
- Long BGM: prefer **stream decode → `playStream`** in Dev; Ship may cook PCM chunked stream or keep compressed stream container inside `.ayaudio` extension later — v1 cook = full PCM or chunked PCM is enough.

**Status (2026-08-13):** A1–A6 shipped in AYResource (`AudioDecoder` + loose loaders + cook). See §2.3.1.

#### 2.3.1 Implementation slices (ordered)

| Slice | Deliverable | Done when |
|-------|-------------|-----------|
| **A0** | Lock docs (this section + AYResource §4.2 audio policy) | ✅ Docs merged |
| **A1** | `AudioDecoder` façade in AYResource: `decodeFile(path) → PcmBuffer` | ✅ `decodeAudioFile` + WAV UT |
| **A2** | MP3 + OGG decode backends behind A1 | ✅ minimp3 + stb_vorbis (garbage reject UT) |
| **A3** | Dev loader: `ResourceManager` can load `.mp3`/`.ogg`/`.wav` → `IAudio` PCM without prior cook | ✅ `AudioLoader` + bootstrap under `AY_AUDIO_LOOSE_FORMATS` |
| **A4** | `AudioConverter` cook: mp3/ogg/wav → `.ayaudio` PCM; CLI/editor “Optimize / Pack” | ✅ Converter + `IConverter::create` / ImportJob wiring |
| **A5** | Build flag / config: `AY_AUDIO_LOOSE_FORMATS` Debug default ON; OFF in Release | ✅ CMake `ON`/`OFF`/`CONFIG` |
| **A6** | Bridge: Dev `IAudio` → `AudioSubSystem::registerClipFromResource` | ✅ Pre-existing bridge + Resource UT green (2026-08-13) |

**Non-goals for these slices:** HRTF, runtime codec inside `AYAudio`, replacing bus mixer, Video sync.

### 2.4 Threading model ✅ (locked)

- **Game thread:** `AudioSubSystem::update()` — listener pose, emitters; public controls update guarded engine state synchronously and enqueue diagnostic breadcrumbs.
- **Audio thread:** miniaudio data callback — try the shared state gate, drain breadcrumbs, mix voices, write interleaved **float32** to device. Contention yields silence; preload before performance.

No OpenAL-style context on game thread.

---

## 3. Data boundaries

### 3.1 Runtime vs resource

```
[Authoring / Dev]   .wav / .mp3 / .ogg
        │
        ├─(Debug/Editor)──► AYResource AudioDecoder ──► IAudio (PCM in RAM)
        │                                                      │
        └─(Pack / Optimize)► AudioConverter ──► .ayaudio (L1 PCM)
                                      │
                                      ▼
[Runtime Ship]  ResourceManager::load<IAudio>(".ayaudio") ──► PCM blob
                                      │
                                      ▼
              AYAudio::registerClip / playStream  ──► VoiceHandle
              (buses: Master / Music / Sfx / Ui / Voice)
```

`AYAudio` **must not** include `AYResource/assetsImpl/Audio.h` in **public** headers exposed to gameplay. Registration API takes plain PCM descriptor:

```cpp
struct AudioClipDesc {
    uint32_t sampleRate;
    uint16_t channels;      // 1 or 2
    uint16_t bitsPerSample; // 16 for v0
    uint32_t frameCount;    // samples per channel
    const void* pcmData;    // interleaved S16 or F32 (format enum TBD)
};
```

### 3.2 Format decision checklist ✅ (locked 2026-08-13; sample-rate 2026-08-13)

- [x] Ship format: **`.ayaudio` PCM** (Release / pack). Loose formats stripped or rejected.
- [x] Dev formats: **WAV + MP3 + OGG** via **AYResource** decode (not inside AYAudio).
- [x] Runtime codec in AYAudio: **no** (rejected as primary).
- [x] Streaming: long music → `playStream` (Dev may decode incrementally; Ship may use cooked chunked PCM).
- [x] **Sample rate policy (LOCKED):** engine / device target **48000 Hz**.
  - **Cook:** `AudioConverter` defaults to resample → 48 kHz S16 PCM in `.ayaudio`.
  - **Runtime `registerClip`:** if clip rate ≠ engine rate, **linear-resample into owned storage** (ClipRegistry). Pitch must not be used as a sample-rate substitute.
  - **Streams:** producer should push at engine rate; stream-decoder path resamples per chunk when needed.
- [ ] Optional future: compressed payload *inside* `.ayaudio` extension chunk (OPUS) — **not v1**.
- [x] Lightweight optional HRTF (ILD approximation when `ListenerParams::hrtfEnabled`) — full SOFA tables remain v3.
- [x] Custom attenuation curve (`AttenuationModel::CustomCurve`) — shipped in v2.0b.
- [x] Long BGM stream-decode → `AudioSubSystem::playStreamingFile` (v2.2).
- [x] ECS `AudioEmitterComponent` / `AudioListenerComponent` + `AudioEmitterSystem` @ priority 600 (v2.2).

---

## 4. Architecture

### 4.1 Layer diagram

```
┌─────────────────────────────────────────────────────────────┐
│  Gameplay / Editor / Script / ECS (optional)                 │
└───────────────────────────┬─────────────────────────────────┘
                            │ Guarded synchronous controls
┌───────────────────────────▼─────────────────────────────────┐
│  AudioEngine                                                 │
│  ├─ ClipRegistry (id → PCM or stream state)                  │
│  ├─ VoicePool (active instances)                             │
│  ├─ BusMixer (Master / Music / Sfx / Ui / Voice)             │
│  ├─ Spatializer (3D params → pan/gain/filter per voice)      │
│  └─ Listener (pose, velocity for doppler)                    │
└───────────────────────────┬─────────────────────────────────┘
                            │ IAudioBackend
┌───────────────────────────▼─────────────────────────────────┐
│  MiniaudioBackend | NullBackend | MockBackend (tests)        │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 Core handles

```cpp
using AudioClipId = uint32_t;
using VoiceHandle = uint32_t;

enum class AudioBus : uint8_t {
    Master = 0,
    Music,
    Sfx,
    Ui,
    Voice,
    Count
};
```

Invalid handles use `0` (same pattern as engine elsewhere).

### 4.3 Bus mixer

Each bus: `gain`, `mute`, `solo` (editor debug).  
**Master** applies after bus sum.  
**Ducking (v2):** when `Voice` on `Voice` bus is active, reduce `Music` bus by configurable dB.

### 4.4 Commands (game thread → audio thread)

```cpp
enum class AudioCommandType : uint8_t {
    Play,
    Stop,
    StopAll,
    SetVoiceVolume,
    SetVoicePitch,
    SetVoiceSpatialParams,  // partial update
    SetBusGain,
    SetMasterGain,
    SetListener,
    RegisterClip,
    ReleaseClip,
    PauseEngine,
    ResumeEngine,
};
```

---

## 5. Spatial audio — tunable realism (3D)

Design goal: **artists and programmers can tune parameters** to approximate real-world behavior without mandating a single distance formula. Defaults should match common game engine behavior (UE-like rolloff family).

### 5.1 `SpatializationParams` (per Voice)

```cpp
struct SpatializationParams {
    // --- Mode ---
    enum class Mode : uint8_t {
        Disabled,     // 2D: use StereoPan only
        World,        // 3D world space
        Relative,     // offset from listener (UI attach)
    };
    Mode mode = Mode::World;

    // --- Position / orientation (world or relative) ---
    ayt::math::FVector3 position{};
    ayt::math::FVector3 velocity{};       // for doppler (v2)

    // --- Distance attenuation ---
    enum class AttenuationModel : uint8_t {
        Inverse,              // 1 / (1 + rolloff * (d - ref))
        Linear,               // clamp(1 - (d - ref) / (max - ref), 0, 1)
        InverseClamped,       // inverse with clamp at max distance
        CustomCurve,          // sample authored curve (v2)
    };
    AttenuationModel attenuation = AttenuationModel::InverseClamped;

    float referenceDistance = 1.0f;       // meters; full volume inside
    float maxDistance       = 100.0f;     // silence / clamp beyond
    float rolloffFactor     = 1.0f;       // curve steepness
    float minGain           = 0.0f;       // floor (audibility)
    float maxGain           = 1.0f;       // ceiling (prevent boost)

    // --- Stereo imaging ---
    float stereoPan         = 0.0f;       // -1..1 when mode=Disabled or blend
    float spatialBlend      = 1.0f;       // 0=2D pan only, 1=full 3D

    // --- Directional cone (sources with facing) ---
    ayt::math::FVector3 forward{0, 0, -1};
    float innerConeAngleDeg = 360.0f;     // 360 = omnidirectional
    float outerConeAngleDeg = 360.0f;
    float outerConeGain     = 0.0f;       // gain outside outer cone

    // --- Air / obstruction (simple realism) ---
    float airAbsorptionGainPerMeter = 0.0f; // high-frequency loss vs distance (v1: single LP)
    float occlusion                 = 0.0f;   // 0=clear, 1=fully occluded
    float obstruction               = 0.0f;   // partial blockage (walls)
    float occlusionLowPassHz        = 800.0f; // cutoff when occluded

    // --- Environment send (v2) ---
    float reverbSend = 0.0f;              // 0..1 to reverb bus
    uint8_t environmentPreset = 0;        // small room / hall / outdoor table

    // --- Spread ---
    float sourceRadius = 0.0f;            // apparent source size (widens stereo)

    // --- Priority / virtualization ---
    uint8_t priority = 128;               // steal quietest low-priority voice if pool full
};
```

### 5.2 `ListenerParams`

```cpp
struct ListenerParams {
    ayt::math::FVector3 position{};
    ayt::math::FVector3 forward{0, 0, -1};
    ayt::math::FVector3 up{0, 1, 0};
    ayt::math::FVector3 velocity{};       // doppler (v2)

    float masterSpatialScale = 1.0f;      // global unit scale (1 unit = 1 meter default)

    // Optional HRTF (v3)
    bool  hrtfEnabled = false;
    const char* hrtfProfile = nullptr;  // "default", file path, etc.
};
```

### 5.3 Processing order (per voice, per frame)

1. Compute distance / relative direction listener ← source  
2. Evaluate **attenuation model** → distance gain  
3. Apply **cone** modulation (direction vs source forward)  
4. Apply **occlusion/obstruction** → gain + low-pass coefficient  
5. Apply **air absorption** (distance-based LP)  
6. Compute **pan** (equal-power pan law) or **HRTF** (v3)  
7. Apply **spatialBlend** with 2D pan  
8. Apply **bus gain** and **master**  
9. Mix into interleaved output buffer  

All parameters exposed to **JSON/component** later; v0 implements subset (§11).

### 5.4 Realism vs performance knobs

| Knob | Effect |
|------|--------|
| `maxAudibleVoices` | Virtualization: stop furthest / lowest priority |
| `spatialUpdateRate` | Update 3D every N audio frames |
| `hrtfEnabled` | Quality vs CPU |
| `reverbSend` | Optional parallel reverb tap |

---

## 6. Public API (sketch)

```cpp
namespace ayt::audio {

class AudioEngine {
public:
    bool initialize(const AudioSettings& settings);
    void shutdown();

    AudioClipId registerClip(const AudioClipDesc& desc);
    void releaseClip(AudioClipId id);

    VoiceHandle play(AudioClipId clip, AudioBus bus,
                     const SpatializationParams& spatial = {},
                     float volume = 1.0f, bool loop = false);
    void stop(VoiceHandle voice);
    void stopAll();

    void setListener(const ListenerParams& listener);
    void setBusGain(AudioBus bus, float linearGain);
    void setMasterGain(float linearGain);

    void pause();
    void resume();

    // Called from AudioSubSystem::update on game thread
    void submitListenerAndCommands(float deltaTime);
};

} // namespace ayt::audio
```

---

## 7. Subsystem lifecycle

```cpp
class AudioSubSystem : public ayt::game::IGameLoop::ISubSystem {
public:
    static void registerSubSystem();  // Client builds only

    bool initialize() override;
    void update(float dt) override;
    void shutdown() override;
};
```

**Shutdown order (with Editor):** stop all voices → close miniaudio device → then allow process teardown.  
Register **before** or **after** `RendererSubSystem` is acceptable; audio does not depend on bgfx.

**Config** (`AYApplication`):

- CLI: `-no-audio`
- INI: `[Audio] EnableAudio=true`, `MasterVolume=0.8`

When disabled: `NullBackend` (no device).

---

## 8. Engine integration (phased)

| Integration | Phase |
|-------------|-------|
| `AudioSubSystem` in `GameLoop` | v0.5 |
| `ResourceManager` → `registerClip` | v1 |
| `Listener` follows `Renderer` main camera | v1 |
| `AudioEmitterComponent` + `AudioEmitterSystem` @ priority 600 | v2.2 ✅ |
| `AudioPlayEvent` on `AYEventSystem` | v2 |
| Sync to animation / VMD timeline | v2 |

---

## 9. Testing strategy

| Test | Backend | Validates |
|------|---------|-----------|
| `Test_AudioMixer.cpp` | None (CPU) | Bus sum, pan law, inverse distance |
| `Test_Spatializer.cpp` | None | Cone, occlusion gain, spatialBlend |
| `Test_AudioCommands.cpp` | MockBackend | Command queue ordering |
| `Test_MiniaudioBackend.cpp` | Optional HW | Smoke; skip if no device |

**Rule:** v0 PR must pass **Mock + CPU** tests without sound card.

---

## 10. Directory layout (target)

```
AYRuntime/AYAudio/
├── design.md                 # this file
├── README.md
├── CMakeLists.txt
├── include/
│   ├── AYAudio.h
│   ├── AYAudio/AudioTypes.h
│   ├── AYAudio/AudioEngine.h
│   └── AYAudio/AudioSubSystem.h
├── interface/
│   └── AYAudio/IAudioBackend.h
├── backend/
│   ├── MiniaudioBackend.cpp
│   ├── NullBackend.cpp
│   └── MockBackend.cpp
├── src/
│   ├── AYAudioEngine.cpp
│   ├── AYAudioMixer.cpp
│   ├── AYAudioSpatializer.cpp
│   ├── AYAudioCommandQueue.cpp
│   └── AYAudioSubSystem.cpp
├── thirdparty/
│   └── miniaudio.h
└── unittest/
    ├── Test_AudioMixer.cpp
    └── Test_Spatializer.cpp
```

---

## 11. Delivery phases

| Phase | Scope | Integration |
|-------|-------|-------------|
| **v0** | `IAudioBackend` Mock, mixer, buses, 2D pan, command queue | None |
| **v0.5** | MiniaudioBackend, `AudioSubSystem`, Null on `-no-audio` | GameLoop |
| **v1** | Full `SpatializationParams` distance/cone/occlusion LP; `Listener` | Camera + Resource PCM register |
| **v1.5** | Streaming buffer, pause/timeScale | GameLoop clock |
| **v2** | ECS emitter, ducking, reverb send (simple algorithmic) | Entity |
| **v2.0b** | Streaming voices + ducking + reverb/doppler polish | Resource bridge (PCM) |
| **v2.1** | **Dev loose WAV/MP3/OGG + cook-on-pack** (§2.3 lock) | AYResource A1–A6 |
| **v2.2** | 48 kHz sample-rate policy + cook/runtime resample; chunked BGM stream-decode; ECS AudioEmitter/Listener | Resource + Entity |
| **v2.3** | Lightweight optional HRTF (ITD/ILD); CustomCurve already in v2.0b | Content |
| **v3** | Full SOFA/HRTF profiles, advanced filters (air/occlusion LP) | Content tools |

**Resource format decision** locked 2026-08-13 (§2.3). v2.1 (A1–A6) shipped in AYResource.

---

## 12. Changelog

| Date | Change |
|------|--------|
| 2026-08-15 | **AYAudio Editor** (AYUI): `AudioEditorSession` + `AYAudio_AudioEditor` + Editor Tools child; Editor registers `AudioSubSystem` |
| 2026-08-15 | **AYAudio_Gallery** demo: Win32 mixer UI (beeps, buses, spatial, duck/reverb/timeScale, stream open) |
| 2026-08-13 | **Implement §2.3.1 A1–A6**: AYResource `AudioDecoder` (WAV/minimp3/stb_vorbis), loose loaders, cook path, `AY_AUDIO_LOOSE_FORMATS`, ImportJob wiring |
| 2026-08-13 | **Lock §2.3**: Dev common formats (WAV/MP3/OGG) in AYResource; cook PCM `.ayaudio` for ship/optimize only; reject codec-in-AYAudio; add §2.3.1 slices A0–A6; update §3.1/§3.2 |
| 2026-07-11 | Initial design: miniaudio, SDL2 decoupling, spatial params, format TBD |

## 2026-10-04: sampler, voice identity and bounded previews

Voice handles now carry monotonic identities rather than reusable pool indices.
All setters resolve identity; late stale handles cannot mutate a replacement.
Engine callbacks and main-thread state mutation share a recursive state gate;
the callback never waits (silence on contention). Device stop joins callbacks
before registry teardown. Already-applied command breadcrumbs are tagged and
ignored on drain; they cannot resurrect an old voice or free a new clip that
reused its slot. Raw diagnostic queue playback commands are not gameplay APIs.
Do not mutate `clipRegistry` or `mixer` directly during live rendering; diagnostics
that expose raw pointers are only stable between engine mutations. Prefer copies.

PCM rendering carries fractional interpolation position across blocks, bounds
playback to [rangeBegin,rangeEnd), and scales fade-out by remaining source frames.
Per-output-frame attack/release state is applied before mixing and committed
with the same voice identity. Pause preserves source/envelope age. Mono compacts
the stereo source scratch; mix scratch holds 4096 stereo frames. Larger engine
render requests are split into blocks. Instruments preload and borrow regions;
the sampler owns notes, sustain and stealing policy, not codecs or UI input.

AudioEditorSession owns all preview clips/voices. Detach releases only those
resources, detects host engine replacement, and unbinds chrome callbacks before
UI destruction. Opening a window does not reset listener/gains/effects. Keyboard
and pointer state share note gates so one source's release cannot stop the other.
Both hosts route key-up and focus loss; numeric/text editing suppresses note-on.

Full-file sample loading uses AYResource AudioLoader. PCM frame count is derived
from validated byte length at the AYAudio bridge, avoiding stereo truncation
when decoder metadata counts per-channel frames. Audio documents use that same
PCM layout to derive source duration.

The default instrument loads cooked C1..C7 .aud assets first, so Release does not
depend on the development loose MP3 loader. Original samples and attribution
remain in AssetRepo; import_tool produces the cooked PCM copied by the manifest.

### Shared workbench and output telemetry (2026-10-04)

Both native hosts load a small mount panel, then the shared session owns a
programmatically constructed workbench subtree. TabControl owns independent
scroll pages; a persistent narrow right column draws the stereo meter. Free
mount anchors explicitly disable Box-managed position/size. Piano is a LeafWidget
over MIDI 21..108 with cursor-anchored width zoom, pan and mouse glissando.
Physical pointer coordinates are converted to UI logical coordinates by hosts.
Changing octave/transpose/tab/routing or losing focus releases held notes.
Transpose is a semitone offset, not a change to source-region roots; unsupported
notes are shaded. Keyboard and pointer note gates combine per actual MIDI pitch.

AudioMixer measures absolute L/R sample peaks after gains/effects and before
clamping. Mono mirrors L into R. AudioEngine publishes approximate independent
atomic display fields and a monotonically increasing callback block serial;
silence/paused/contended callbacks and shutdown clear readings. Render blocks
larger than scratch capacity aggregate chunk peaks. The UI applies visual decay
and a short overload hold, clears stale readings, and labels dBFS. This telemetry
is neither a coherent voice-state snapshot nor integrated loudness measurement.

Resource waveforms use validated decoded PCM and own clip/voice registrations.
The editor uses the existing timeline-v2 fields, not a new format. One clip at
timeline 0 stores sourceOffset/duration/gain/fades; timeline preview starts at 0,
so the main document does not apply sourceOffset twice. Read-only fallback
protects unsupported schemas, keyframes, multiple clips and nonzero timeline
starts. Atomic sidecar writes preserve original audio. Fade durations are bounded
to source selection length when dragging. Undo/redo stores validated edits;
changing resource routes stops only that resource voice. Preset library, export,
envelope authoring and a dedicated Preview bus are visibly disabled.

### Native workbench layout cache repair (2026-10-04)

The native child loads and lays out the mount panel before attaching the session.
Programmatic widget insertion does not invalidate UIManager layout caches, and
the GDI child tick loop updates/renders without a layout pass. Attachment must
activate its own UIManager, invalidate the cache and perform layout immediately.
Tab selection must also invalidate and perform layout immediately. Tests must
use the production load order without a post-attachment resize/layout call,
including a different primary UIManager being active and repeated attachment.

### Performance shortcuts and compact telemetry (2026-10-04)

Physical keyboard gates latch actual note-on MIDI pitches in a 256-entry table.
Left Shift/Alt temporary offsets are +12/-12 semitones (one octave), compose additively and
affect new presses; key-up uses the latched pitch even if modifiers/focus have
changed. Auto-repeat does not create voices. Multiple physical bindings for
one pitch share the keyboard gate, so releasing upper A cannot stop lower
comma while it remains held. Pointer releases similarly retain the note-on
pitch. Focus loss, mapping changes, detach and host replacement clear latches
and temporary state. Native hosts preserve left/right modifier identity; the
standalone resolves Shift scancodes with a Left Shift fallback when absent.
Lower Z X C V B N M comma map to offsets -12,-10,-8,-7,-5,-3,-1,0 from base C.

The persistent meter moves into the 80-pixel header with a 150-pixel width,
removing the full-height sidebar. Narrow vertical L/R segments, peak hold and
overload remain; scale ticks are 0/-30/-60. Below-floor readings show -inf.
Piano pitch and shortcut labels render separately because native backends
do not all interpret embedded newlines; the initial strip starts at C3.

The host CMake directories explicitly list AudioWorkbench.inl as an
OBJECT_DEPENDS of AudioEditorSession.cpp so included UI changes trigger a
recompile even when compiler include-dependency scanning is unavailable.
