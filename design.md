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
| Replace **AYGraph AudioGraph** (node-based authoring) | Future layer on top of AYAudio buses |
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

### 2.3 Resource / decode path: **TBD** (open)

Runtime needs PCM. Source of PCM is **not finalized**:

| Strategy | Pros | Cons | Phase suggestion |
|----------|------|------|------------------|
| **A. Cook-only** — content pipeline converts to `.ayaudio`; runtime only loads `IAudio` | Stable, fast load, ship-friendly | Extra cook step; no “drop WAV in folder” |
| **B. Dev loose WAV** — `AYAudio` or `AYResource` loads `.wav` in **dev** builds only | Fast iteration | Two paths; ship must strip |
| **C. Runtime decode** — MP3/OGG via `minimp3` / `stb_vorbis` inside `AYAudio` | No cook for music | CPU spike on play; licensing/size; threading |

**Current codebase:** `AYResource` has `.ayaudio` + `AudioConverter` (WAV → cook). **v0 runtime assumes cooked PCM** via `registerClipFromPcm()`.

**Decision gate (before v1 ship):** pick A only, or A+B (dev), or A+C (streaming music). Document in §3.1 when decided.

### 2.4 Threading model ✅ (locked)

- **Game thread:** `AudioSubSystem::update()` — listener pose, emitters, enqueue `AudioCommand`s.
- **Audio thread:** miniaudio data callback — drain commands, mix voices, write interleaved **float32** to device.

No OpenAL-style context on game thread.

---

## 3. Data boundaries

### 3.1 Runtime vs resource

```
[Authoring]  WAV/MP3/OGG  ──►  AYResource AudioConverter  ──►  .ayaudio (L1)
                                      │
                                      ▼
[Runtime]    AYResourceManager::load<IAudio>()  ──►  PCM blob + metadata
                                      │
                                      ▼
              AYAudio::registerClip()  ──►  AudioClipId (engine handle)
                                      │
                                      ▼
              AudioEngine::play(clipId, params)  ──►  VoiceHandle
```

`AYAudio` **must not** include `assetsImpl/AYAudio.h` in **public** headers exposed to gameplay. Registration API takes plain PCM descriptor:

```cpp
struct AudioClipDesc {
    uint32_t sampleRate;
    uint16_t channels;      // 1 or 2
    uint16_t bitsPerSample; // 16 for v0
    uint32_t frameCount;    // samples per channel
    const void* pcmData;    // interleaved S16 or F32 (format enum TBD)
};
```

### 3.2 Format decision checklist (fill when decided)

- [ ] Ship format: `.ayaudio` only vs loose WAV dev override
- [ ] Runtime MP3/OGG: yes/no; if yes, which library and thread
- [ ] Streaming vs fully resident for music (>10 MB)
- [ ] Sample rate policy: force 48 kHz device vs resample on register

---

## 4. Architecture

### 4.1 Layer diagram

```
┌─────────────────────────────────────────────────────────────┐
│  Gameplay / Editor / Script / ECS (optional)                 │
└───────────────────────────┬─────────────────────────────────┘
                            │ AudioCommand queue (SPSC)
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
| `AudioEmitterComponent` + `AudioSystem` @ priority 600+ | v2 |
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
│   ├── AYAudioTypes.h
│   ├── AYAudioEngine.h
│   └── AYAudioSubSystem.h
├── interface/
│   └── IAudioBackend.h
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
| **v3** | HRTF optional, custom attenuation curves | Content tools |

**Resource format decision** does not block v0–v0.5.

---

## 12. Changelog

| Date | Change |
|------|--------|
| 2026-07-11 | Initial design: miniaudio, SDL2 decoupling, spatial params, format TBD |
