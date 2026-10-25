# AYAudioPlayback

Lightweight link target for the existing AudioEngine, mixer, spatializer, clip
registry, PCM streams, effects and Null/Mock/Miniaudio backends. Add this directory
after AYCore and Foundation; provide `MINIAUDIO_INCLUDE_DIRS` or an installed
miniaudio header. ZLIB supplies the existing libmysofa dependency. No GameLoop,
ECS, resource loader or subsystem is required. AYAudio links this same core and
adds its content/resource/ECS/runtime wrappers.

All control methods run on one owner thread; the backend owns its render callback.
Stop producers and close their streams before engine teardown. Live voice panning
uses independent L/R gains after duplicating mono sources; it works for both mono
clips and streams. The optional header-only AYAudioPan target exposes the same
equal-power gain calculation without initializing a device.

Streams default to advancing their media cursor through underflow. For queued
speech set `AudioStreamDesc::advanceClockOnUnderrun=false`: missing samples become
silence and leave the source cursor unchanged until PCM resumes. Declare EOS when
the producer is done. This does not replace producer queue budgets or cancellation.

`AYAudioPlayback_Tests` exercises stream direction changes, source cursor holding,
clip resampling and repeated lifecycle teardown using a deterministic Null device.
`AYAudio_PanTests` covers the gain curve, bounds and non-finite inputs. Physical
device audibility and latency remain integration/manual checks.
