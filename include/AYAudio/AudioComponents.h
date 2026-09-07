#pragma once

#include <AYEntity/ComponentRegistry.h>

namespace ayt::audio
{

// Registers the ECS component types owned by AYAudio. Module hosts call the
// registry overload during IModule::registerTypes(), before ComponentRegistry
// is sealed. The no-argument overload is retained for small direct hosts and
// tests that do not assemble an AYModule graph.
[[nodiscard]] ayt::entity::ComponentRegistryResult registerAudioComponents(
    ayt::entity::ComponentRegistry& registry);
void registerAudioComponents();

} // namespace ayt::audio
