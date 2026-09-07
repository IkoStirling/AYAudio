#include <AYAudio/AudioRuntimeModule.h>
#include <AYAudio/components/AudioEmitterComponent.h>
#include <AYAudio/components/AudioListenerComponent.h>
#include <AYEntity/ComponentRegistry.h>
#include <AYModule/ModuleContext.h>
#include <AYModule/ModuleManager.h>
#include <AYTest.h>

#include <string>

namespace ayt::audio::test
{

TEST_SUITE(AudioRuntimeModuleTests)

TEST_CASE(registers_audio_components_in_the_context_registry)
{
    ayt::entity::ComponentRegistry registry;
    ayt::module::ModuleContext context;
    CHECK_TRUE(context.bindService(
        std::string(ayt::entity::kComponentRegistryModuleService),
        &registry));

    ayt::module::ModuleManager modules;
    CHECK_TRUE(modules.emplace<AudioRuntimeModule>().succeeded());
    CHECK_TRUE(modules.resolve().succeeded());
    CHECK_TRUE(modules.registerTypes(context).succeeded());

    CHECK_NOT_NULL(registry.find<AudioEmitterComponent>());
    CHECK_NOT_NULL(registry.find<AudioListenerComponent>());
    CHECK(registry.size() == 2);
}

TEST_CASE(rejects_type_registration_without_a_component_registry_service)
{
    ayt::module::ModuleContext context;
    ayt::module::ModuleManager modules;
    CHECK_TRUE(modules.emplace<AudioRuntimeModule>().succeeded());
    CHECK_TRUE(modules.resolve().succeeded());

    const auto result = modules.registerTypes(context);
    CHECK_FALSE(result.succeeded());
    CHECK(result.code() == ayt::module::ModuleErrorCode::TypeRegistrationFailed);
}

TEST_SUITE_END

} // namespace ayt::audio::test
