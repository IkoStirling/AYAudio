#include <AYAudio/AudioBackendFactory.h>

namespace ayt::audio
{

std::unique_ptr<IAudioBackend> makeBackend(AudioBackendKind kind) {
    switch (kind) {
        case AudioBackendKind::Null:      return makeNullBackend();
        case AudioBackendKind::Mock:      return makeMockBackend();
        case AudioBackendKind::Miniaudio: return makeMiniaudioBackend();
    }
    return makeNullBackend();
}

} // namespace ayt::audio