#pragma once
// AYAudio.h - Module umbrella include for AYAudio
//
// Convenience header that pulls in the public AYAudio surface. Game code
// can simply `#include <AYAudio.h>` and get every type / class needed to
// play sounds.

#include <AYAudioTypes.h>
#include <AYAudioCommandQueue.h>
#include <AYAudioMixer.h>
#include <AYAudioSpatializer.h>
#include <AYAudioClipRegistry.h>
#include <AYAudioEngine.h>
#include <AYAudioSubSystem.h>
#include <AYAudioEmitterSystem.h>
#include <AYAudioBackendFactory.h>
#include <components/AYAudioEmitterComponent.h>
#include <components/AYAudioListenerComponent.h>