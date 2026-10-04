#pragma once
// AYAudio.h - Module umbrella include for AYAudio
//
// Convenience header that pulls in the public AYAudio surface. Game code
// can simply `#include <AYAudio.h>` and get every type / class needed to
// play sounds.

#include <AYAudio/AudioTypes.h>
#include <AYAudio/AudioCommandQueue.h>
#include <AYAudio/AudioMixer.h>
#include <AYAudio/AudioSpatializer.h>
#include <AYAudio/AudioClipRegistry.h>
#include <AYAudio/AudioEngine.h>
#include <AYAudio/SampleInstrument.h>
#include <AYAudio/AudioComponents.h>
#include <AYAudio/AudioSubSystem.h>
#include <AYAudio/AudioEmitterSystem.h>
#include <AYAudio/AudioBackendFactory.h>
#include <AYAudio/components/AudioEmitterComponent.h>
#include <AYAudio/components/AudioListenerComponent.h>
