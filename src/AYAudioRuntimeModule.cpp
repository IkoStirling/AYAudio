#include <AYAudio/AudioRuntimeModule.h>

#include <AYAudio/AudioComponents.h>
#include <AYAudio/AudioSubSystem.h>

#include <memory>
#include <string>
#include <utility>

namespace ayt::audio
{

AudioRuntimeModule::AudioRuntimeModule(
    RuntimeAudioBackendFactory backendFactory)
    : SubSystemModule(
          ayt::module::ModuleDescriptor{
              .id = std::string(kAudioRuntimeModuleId),
              .displayName = "AYAudio Runtime",
              .version = "1.0.0",
              .dependencies = {
                  ayt::module::ModuleDependency::optional(
                      "AYEntity.Runtime")}},
          "Audio",
          [backendFactory = std::move(backendFactory)]() {
              auto system = std::make_unique<AudioSubSystem>();
              if (backendFactory) {
                  system->setBackend(backendFactory());
              }
              return system;
          })
{
}

ayt::module::ModuleResult AudioRuntimeModule::registerTypes(
    ayt::module::IModuleContext& context)
{
    auto* registry = context.findServiceAs<ayt::entity::ComponentRegistry>(
        ayt::entity::kComponentRegistryModuleService);
    if (registry == nullptr) {
        return ayt::module::ModuleResult::failure(
            ayt::module::ModuleErrorCode::TypeRegistrationFailed,
            "ComponentRegistry service is unavailable for AYAudio.Runtime");
    }

    ayt::entity::ComponentRegistryResult result = registerAudioComponents(
        *registry);
    if (!result) {
        return ayt::module::ModuleResult::failure(
            ayt::module::ModuleErrorCode::TypeRegistrationFailed,
            result.message());
    }
    return ayt::module::ModuleResult::success();
}

} // namespace ayt::audio
