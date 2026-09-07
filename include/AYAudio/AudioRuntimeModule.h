#pragma once

#include <AYAudio/IAudioBackend.h>
#include <AYGameLoop/SubSystemModule.h>

#include <functional>
#include <memory>
#include <string_view>

namespace ayt::audio
{

inline constexpr std::string_view kAudioRuntimeModuleId =
    "AYAudio.Runtime";

using RuntimeAudioBackendFactory =
    std::function<std::unique_ptr<IAudioBackend>()>;

class AudioRuntimeModule final : public ayt::game::SubSystemModule
{
public:
    explicit AudioRuntimeModule(
        RuntimeAudioBackendFactory backendFactory = {});

    [[nodiscard]] ayt::module::ModuleResult registerTypes(
        ayt::module::IModuleContext& context) override;
};

} // namespace ayt::audio
