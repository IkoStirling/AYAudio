#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace ayt::audio {
enum class OutputQueryStatus { Ok, Partial, Unavailable, Unsupported };
enum class OutputSessionState { Inactive, Active, Expired, Unknown };
struct OutputVolume {
    bool known=false,muted=false;
    float master=0;
    std::vector<float> channels;
};
struct OutputEndpoint {
    bool available=false;
    std::string name;
    OutputVolume volume;
};
struct ProcessOutputSession {
    std::string endpointName;
    bool defaultForConsole=false,defaultForMultimedia=false;
    OutputSessionState state=OutputSessionState::Unknown;
    OutputVolume session,endpoint;
};
struct OutputDiagnostics {
    OutputQueryStatus status=OutputQueryStatus::Unsupported;
    OutputEndpoint console,multimedia;
    std::vector<ProcessOutputSession> processSessions;
    bool truncated=false;
};
/// Read-only Windows endpoint/session observation for the current process only.
/// No recording, device selection, volume/mute changes, or other-process metadata.
/// Call on a non-render worker; COM is scoped when not already initialized. A
/// bounded one-time enumeration can miss newly created/shared-process sessions.
/// Missing/unknown observations never prove silence or that a person heard audio.
/// Unsupported platforms return Unsupported and empty observations.
OutputDiagnostics queryProcessOutput();
}
