// AYAudioGallery.cpp — standalone AYAudio mixer / playback gallery.
//
// Win32 host (AYVideo EngineDemo style): knobs for one-shots, buses,
// spatial beeps, ducking, reverb, timeScale, and optional streamed file.
// No AYUI / bgfx — audio-only acceptance surface.

#ifndef UNICODE
#  define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>
#include <CommCtrl.h>
#include <commdlg.h>

#include <AYResource/assetsImpl/Audio.h>
#include <AYAudio/AudioBackendFactory.h>
#include <AYAudio/AudioSubSystem.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace
{

void galleryLog(const char* fmt, ...)
{
    std::fprintf(stderr, "[AYAudioGallery] ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

void ensureConsole()
{
    if (GetConsoleWindow() != nullptr)
    {
        return;
    }
    if (!AllocConsole())
    {
        return;
    }
    FILE* out = nullptr;
    freopen_s(&out, "CONOUT$", "w", stdout);
    freopen_s(&out, "CONOUT$", "w", stderr);
    SetConsoleTitleW(L"AYAudio Gallery — log");
}

constexpr int kWindowW = 900;
constexpr int kWindowH = 560;
constexpr UINT_PTR kTimerId = 1;

constexpr int IDC_PLAY_A     = 2001;
constexpr int IDC_PLAY_B     = 2002;
constexpr int IDC_PLAY_LOOP  = 2003;
constexpr int IDC_STOP_ALL   = 2004;
constexpr int IDC_PAUSE      = 2005;
constexpr int IDC_OPEN_STREAM = 2006;
constexpr int IDC_STOP_STREAM = 2007;
constexpr int IDC_SPATIAL_L  = 2008;
constexpr int IDC_SPATIAL_C  = 2009;
constexpr int IDC_SPATIAL_R  = 2010;
constexpr int IDC_DUCK       = 2011;
constexpr int IDC_REVERB_PRE = 2012;
constexpr int IDC_MASTER     = 2100;
constexpr int IDC_MUSIC      = 2101;
constexpr int IDC_SFX        = 2102;
constexpr int IDC_REVERB     = 2103;
constexpr int IDC_TIMESCALE  = 2104;
constexpr int IDC_STATUS     = 2200;

struct GalleryState
{
    HWND hwnd = nullptr;
    HWND status = nullptr;
    bool running = true;

    ayt::audio::AudioSubSystem audio;
    ayt::audio::AudioClipId clipA = ayt::audio::InvalidClipId;
    ayt::audio::AudioClipId clipB = ayt::audio::InvalidClipId;
    ayt::audio::AudioClipId clipLoop = ayt::audio::InvalidClipId;
    ayt::audio::VoiceHandle loopVoice = ayt::audio::InvalidVoice;
    ayt::audio::StreamingPlayback stream{};

    // Owned PCM for registered clips (engine holds pointers).
    std::vector<float> pcmA;
    std::vector<float> pcmB;
    std::vector<float> pcmLoop;

    bool ducked = false;
    uint8_t reverbPreset = 0;
    bool paused = false;
    uint32_t tick = 0;
};

void setStatus(GalleryState& s, const std::wstring& text)
{
    if (s.status)
    {
        SetWindowTextW(s.status, text.c_str());
    }
}

std::vector<float> makeSine(uint32_t sampleRate, uint16_t channels,
                            float freqHz, float seconds, float amp)
{
    const uint32_t frames =
        static_cast<uint32_t>(static_cast<double>(sampleRate) * seconds);
    std::vector<float> pcm(static_cast<size_t>(frames) * channels, 0.0f);
    const double twoPiF = 2.0 * 3.14159265358979323846 * static_cast<double>(freqHz);
    for (uint32_t i = 0; i < frames; ++i)
    {
        const float s = static_cast<float>(
            std::sin(twoPiF * static_cast<double>(i)
                     / static_cast<double>(sampleRate))
            * amp);
        for (uint16_t c = 0; c < channels; ++c)
        {
            pcm[static_cast<size_t>(i) * channels + c] = s;
        }
    }
    return pcm;
}

ayt::audio::AudioClipId registerOwnedClip(GalleryState& s,
                                          std::vector<float>& storage,
                                          float freqHz, float seconds,
                                          float amp)
{
    ayt::audio::AudioEngine* eng = s.audio.engine();
    if (!eng)
    {
        return ayt::audio::InvalidClipId;
    }
    storage = makeSine(eng->outputSampleRate(), 1, freqHz, seconds, amp);
    ayt::audio::AudioClipDesc desc{};
    desc.sampleRate = eng->outputSampleRate();
    desc.channels = 1;
    desc.bitsPerSample = 32;
    desc.format = ayt::audio::AudioSampleFormat::F32;
    desc.frameCount = static_cast<uint32_t>(storage.size());
    desc.pcmData = storage.data();
    return eng->registerClip(desc);
}

bool initAudio(GalleryState& s)
{
    galleryLog("init: installing Miniaudio backend…");
    s.audio.setBackend(ayt::audio::makeMiniaudioBackend());
    if (!s.audio.initialize())
    {
        galleryLog("init: AudioSubSystem::initialize FAILED");
        setStatus(s, L"Audio init failed (no device?) — see console log");
        return false;
    }
    ayt::audio::AudioEngine* eng = s.audio.engine();
    if (!eng || !eng->isInitialized())
    {
        galleryLog("init: engine null / not initialized");
        setStatus(s, L"Engine not initialized");
        return false;
    }

    const bool real = eng->backend() && eng->backend()->isRealDevice();
    galleryLog("init: ok backend=%s sampleRate=%u ch=%u master=0.8",
               real ? "Miniaudio(real)" : "Null/Mock",
               eng->outputSampleRate(),
               static_cast<unsigned>(eng->outputChannels()));

    s.clipA = registerOwnedClip(s, s.pcmA, 440.0f, 0.35f, 0.35f);
    s.clipB = registerOwnedClip(s, s.pcmB, 880.0f, 0.25f, 0.30f);
    s.clipLoop = registerOwnedClip(s, s.pcmLoop, 220.0f, 1.0f, 0.20f);
    galleryLog("init: clips A=%u B=%u Loop=%u (frames A=%zu)",
               s.clipA, s.clipB, s.clipLoop, s.pcmA.size());

    eng->setMasterGain(1.0f);
    eng->setBusGain(ayt::audio::AudioBus::Music, 1.0f);
    eng->setBusGain(ayt::audio::AudioBus::Sfx, 1.0f);
    eng->setMasterReverbReturn(0.15f);
    eng->setReverbPreset(0);
    eng->setDopplerScale(1.0f);

    ayt::audio::ListenerParams listener{};
    listener.position = {0, 0, 0};
    listener.forward = {0, 0, -1};
    listener.up = {0, 1, 0};
    eng->setListener(listener);

    setStatus(s,
              L"How-to: Play 440 first (device check) → Open Stream… (mp3/wav/ogg) "
              L"→ Music slider. Console has logs.");
    return true;
}

void refreshStatus(GalleryState& s)
{
    ayt::audio::AudioEngine* eng = s.audio.engine();
    if (!eng)
    {
        return;
    }
    wchar_t buf[256];
    std::swprintf(buf, 256,
                  L"voices=%u  underruns=%llu  scale=%.2f  duck=%s  reverbP=%u  "
                  L"paused=%s  stream=%s",
                  eng->activeVoiceCount(),
                  static_cast<unsigned long long>(eng->streamUnderrunCount()),
                  eng->timeScale(), s.ducked ? L"on" : L"off",
                  static_cast<unsigned>(s.reverbPreset),
                  s.paused ? L"yes" : L"no",
                  s.stream.voice != ayt::audio::InvalidVoice ? L"yes" : L"no");
    setStatus(s, buf);
}

void playBeep(GalleryState& s, ayt::audio::AudioClipId clip,
              ayt::audio::AudioBus bus)
{
    ayt::audio::AudioEngine* eng = s.audio.engine();
    if (!eng || clip == ayt::audio::InvalidClipId)
    {
        return;
    }
    (void)eng->play(clip, bus, {}, 1.0f, false);
    refreshStatus(s);
}

void playSpatial(GalleryState& s, float x)
{
    ayt::audio::AudioEngine* eng = s.audio.engine();
    if (!eng || s.clipA == ayt::audio::InvalidClipId)
    {
        return;
    }
    ayt::audio::SpatializationParams sp{};
    sp.mode = ayt::audio::SpatializationParams::Mode::World;
    sp.position = {x, 0, -2};
    sp.referenceDistance = 1.0f;
    sp.maxDistance = 40.0f;
    sp.spatialBlend = 1.0f;
    sp.reverbSend = 0.35f;
    (void)eng->play(s.clipA, ayt::audio::AudioBus::Sfx, sp, 1.0f, false);
    refreshStatus(s);
}

std::wstring pickAudioFile(HWND owner)
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter =
        L"Audio (*.wav;*.mp3;*.ogg)\0*.wav;*.mp3;*.ogg\0All\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
    {
        return {};
    }
    return file;
}

// AYIO File uses CreateFileA — pass ACP (not UTF-8) or non-ASCII paths fail.
std::string narrowForIo(const std::wstring& w)
{
    if (w.empty())
    {
        return {};
    }
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0)
    {
        return {};
    }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::string narrowUtf8Log(const std::wstring& w)
{
    if (w.empty())
    {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0)
    {
        return {};
    }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

HWND addButton(HWND parent, int id, const wchar_t* text, int x, int y, int w,
               int h, HINSTANCE inst)
{
    return CreateWindowW(L"BUTTON", text,
                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, y, w, h,
                         parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         inst, nullptr);
}

HWND addTrack(HWND parent, int id, int x, int y, int w, int h, HINSTANCE inst,
              int maxVal = 100)
{
    HWND t = CreateWindowW(TRACKBAR_CLASSW, L"",
                           WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, x, y, w, h,
                           parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           inst, nullptr);
    SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(0, maxVal));
    SendMessageW(t, TBM_SETPOS, TRUE, maxVal);
    return t;
}

void createUi(GalleryState& s, HINSTANCE inst)
{
    constexpr int pad = 12;
    int y = pad;
    addButton(s.hwnd, IDC_PLAY_A, L"Play 440 Hz", pad, y, 110, 28, inst);
    addButton(s.hwnd, IDC_PLAY_B, L"Play 880 Hz", pad + 120, y, 110, 28, inst);
    addButton(s.hwnd, IDC_PLAY_LOOP, L"Loop 220 Hz", pad + 240, y, 110, 28,
              inst);
    addButton(s.hwnd, IDC_STOP_ALL, L"Stop All", pad + 360, y, 90, 28, inst);
    addButton(s.hwnd, IDC_PAUSE, L"Pause", pad + 460, y, 80, 28, inst);

    y += 40;
    addButton(s.hwnd, IDC_OPEN_STREAM, L"Open Stream…", pad, y, 130, 28, inst);
    addButton(s.hwnd, IDC_STOP_STREAM, L"Stop Stream", pad + 140, y, 110, 28,
              inst);
    addButton(s.hwnd, IDC_SPATIAL_L, L"Spatial L", pad + 280, y, 90, 28, inst);
    addButton(s.hwnd, IDC_SPATIAL_C, L"Spatial C", pad + 380, y, 90, 28, inst);
    addButton(s.hwnd, IDC_SPATIAL_R, L"Spatial R", pad + 480, y, 90, 28, inst);

    y += 40;
    addButton(s.hwnd, IDC_DUCK, L"Duck Music", pad, y, 110, 28, inst);
    addButton(s.hwnd, IDC_REVERB_PRE, L"Reverb Preset", pad + 120, y, 120, 28,
              inst);

    auto label = [&](const wchar_t* text, int lx, int ly) {
        CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, lx, ly, 90, 20,
                      s.hwnd, nullptr, inst, nullptr);
    };

    y += 44;
    label(L"Master", pad, y + 4);
    HWND master = addTrack(s.hwnd, IDC_MASTER, pad + 90, y, 220, 30, inst);
    SendMessageW(master, TBM_SETPOS, TRUE, 100);

    label(L"Music", pad + 330, y + 4);
    HWND music = addTrack(s.hwnd, IDC_MUSIC, pad + 420, y, 220, 30, inst);
    SendMessageW(music, TBM_SETPOS, TRUE, 100);

    y += 44;
    label(L"SFX", pad, y + 4);
    HWND sfx = addTrack(s.hwnd, IDC_SFX, pad + 90, y, 220, 30, inst);
    SendMessageW(sfx, TBM_SETPOS, TRUE, 100);

    label(L"Reverb", pad + 330, y + 4);
    HWND rev = addTrack(s.hwnd, IDC_REVERB, pad + 420, y, 220, 30, inst);
    SendMessageW(rev, TBM_SETPOS, TRUE, 15);

    y += 44;
    label(L"TimeScale", pad, y + 4);
    HWND ts = addTrack(s.hwnd, IDC_TIMESCALE, pad + 90, y, 340, 30, inst, 400);
    // 100 = 1.0x; range 25..400 maps to 0.25..4.0
    SendMessageW(ts, TBM_SETRANGE, TRUE, MAKELPARAM(25, 400));
    SendMessageW(ts, TBM_SETPOS, TRUE, 100);

    y += 50;
    s.status = CreateWindowW(L"STATIC", L"Starting…",
                             WS_CHILD | WS_VISIBLE | SS_LEFT, pad, y,
                             kWindowW - pad * 2, 40, s.hwnd,
                             reinterpret_cast<HMENU>(
                                 static_cast<INT_PTR>(IDC_STATUS)),
                             inst, nullptr);

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    EnumChildWindows(
        s.hwnd,
        [](HWND child, LPARAM lp) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(font));
}

void onCommand(GalleryState& s, int id)
{
    ayt::audio::AudioEngine* eng = s.audio.engine();
    if (!eng)
    {
        return;
    }
    switch (id)
    {
    case IDC_PLAY_A:
        playBeep(s, s.clipA, ayt::audio::AudioBus::Sfx);
        break;
    case IDC_PLAY_B:
        playBeep(s, s.clipB, ayt::audio::AudioBus::Ui);
        break;
    case IDC_PLAY_LOOP:
        if (s.loopVoice != ayt::audio::InvalidVoice)
        {
            eng->stop(s.loopVoice);
            s.loopVoice = ayt::audio::InvalidVoice;
        }
        s.loopVoice = eng->play(s.clipLoop, ayt::audio::AudioBus::Music, {},
                                0.7f, true);
        refreshStatus(s);
        break;
    case IDC_STOP_ALL:
        eng->stopAll();
        s.loopVoice = ayt::audio::InvalidVoice;
        if (s.stream.voice != ayt::audio::InvalidVoice)
        {
            s.audio.stopStreaming(s.stream);
            s.stream = {};
        }
        refreshStatus(s);
        break;
    case IDC_PAUSE:
        if (s.paused)
        {
            eng->resume();
            s.paused = false;
        }
        else
        {
            eng->pause();
            s.paused = true;
        }
        refreshStatus(s);
        break;
    case IDC_OPEN_STREAM:
    {
        const std::wstring pathW = pickAudioFile(s.hwnd);
        if (pathW.empty())
        {
            galleryLog("stream: file dialog cancelled");
            break;
        }
        const DWORD attr = GetFileAttributesW(pathW.c_str());
        galleryLog("stream: pick utf8=\"%s\" exists=%d",
                   narrowUtf8Log(pathW).c_str(),
                   attr != INVALID_FILE_ATTRIBUTES ? 1 : 0);
        if (attr == INVALID_FILE_ATTRIBUTES)
        {
            setStatus(s, L"File not found: " + pathW);
            galleryLog("stream: GetFileAttributesW failed err=%lu",
                       GetLastError());
            break;
        }
        const std::string pathA = narrowForIo(pathW);
        galleryLog("stream: acp path=\"%s\" bytes=%zu", pathA.c_str(),
                   pathA.size());
        if (pathA.empty())
        {
            setStatus(s, L"Path convert failed (ACP)");
            galleryLog("stream: ACP narrow failed");
            break;
        }

        if (s.stream.voice != ayt::audio::InvalidVoice)
        {
            galleryLog("stream: stopping previous voice=%u", s.stream.voice);
            s.audio.stopStreaming(s.stream);
            s.stream = {};
        }
        // Ambient beds often have quiet masters; subsystem applies makeup
        // from measured content peak. Start at 1.5 as a mild boost.
        s.stream = s.audio.playStreamingFile(pathA, ayt::audio::AudioBus::Music,
                                             true, 1.5f);
        if (s.stream.voice == ayt::audio::InvalidVoice)
        {
            galleryLog("stream: playStreamingFile FAILED (decoder/open). "
                       "Check extension is .mp3/.wav/.ogg and path is ACP-readable.");
            setStatus(s,
                      L"Stream open failed — see console. Prefer ASCII path "
                      L"or check file.");
        }
        else
        {
            ayt::audio::AudioEngine* eng2 = s.audio.engine();
            if (eng2)
            {
                eng2->setMasterGain(1.0f);
                eng2->setBusGain(ayt::audio::AudioBus::Music, 1.0f);
                // Immediate audible cue so Open Stream always “reacts”.
                if (s.clipA != ayt::audio::InvalidClipId)
                {
                    (void)eng2->play(s.clipA, ayt::audio::AudioBus::Sfx, {},
                                     0.55f, false);
                }
            }
            galleryLog("stream: OK voice=%u stream=%u. Listen for short beep "
                       "then BGM. Console: contentPeak / stream.pump peak=",
                       s.stream.voice, s.stream.stream);
            setStatus(s, L"Streaming (+cue beep): " + pathW);
        }
        refreshStatus(s);
        break;
    }
    case IDC_STOP_STREAM:
        if (s.stream.voice != ayt::audio::InvalidVoice)
        {
            s.audio.stopStreaming(s.stream);
            s.stream = {};
        }
        refreshStatus(s);
        break;
    case IDC_SPATIAL_L:
        playSpatial(s, -4.0f);
        break;
    case IDC_SPATIAL_C:
        playSpatial(s, 0.0f);
        break;
    case IDC_SPATIAL_R:
        playSpatial(s, 4.0f);
        break;
    case IDC_DUCK:
        s.ducked = !s.ducked;
        if (s.ducked)
        {
            eng->setDucking(-12.0f, 40.0f, 8.0f);
        }
        else
        {
            eng->setDucking(0.0f, 40.0f, 8.0f);
        }
        refreshStatus(s);
        break;
    case IDC_REVERB_PRE:
        s.reverbPreset = static_cast<uint8_t>((s.reverbPreset + 1) % 2);
        eng->setReverbPreset(s.reverbPreset);
        refreshStatus(s);
        break;
    default:
        break;
    }
}

void onScroll(GalleryState& s, HWND track)
{
    ayt::audio::AudioEngine* eng = s.audio.engine();
    if (!eng || !track)
    {
        return;
    }
    const int id = GetDlgCtrlID(track);
    const int pos =
        static_cast<int>(SendMessageW(track, TBM_GETPOS, 0, 0));
    switch (id)
    {
    case IDC_MASTER:
        eng->setMasterGain(static_cast<float>(pos) / 100.0f);
        break;
    case IDC_MUSIC:
        eng->setBusGain(ayt::audio::AudioBus::Music,
                        static_cast<float>(pos) / 100.0f);
        break;
    case IDC_SFX:
        eng->setBusGain(ayt::audio::AudioBus::Sfx,
                        static_cast<float>(pos) / 100.0f);
        break;
    case IDC_REVERB:
        eng->setMasterReverbReturn(static_cast<float>(pos) / 100.0f);
        break;
    case IDC_TIMESCALE:
        eng->setTimeScale(static_cast<float>(pos) / 100.0f);
        break;
    default:
        break;
    }
    refreshStatus(s);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    GalleryState* s =
        reinterpret_cast<GalleryState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_COMMAND:
        if (s && HIWORD(wParam) == BN_CLICKED)
        {
            onCommand(*s, LOWORD(wParam));
            return 0;
        }
        break;
    case WM_HSCROLL:
        if (s && lParam)
        {
            onScroll(*s, reinterpret_cast<HWND>(lParam));
            return 0;
        }
        break;
    case WM_TIMER:
        if (s && wParam == kTimerId)
        {
            s->audio.update(1.0f / 60.0f);
            ++s->tick;
            if (s->stream.voice != ayt::audio::InvalidVoice
                && (s->tick % 60) == 0)
            {
                ayt::audio::AudioEngine* eng = s->audio.engine();
                if (eng)
                {
                    galleryLog(
                        "stream.tick voices=%u underruns=%llu paused=%d",
                        eng->activeVoiceCount(),
                        static_cast<unsigned long long>(
                            eng->streamUnderrunCount()),
                        s->paused ? 1 : 0);
                }
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        if (s)
        {
            s->running = false;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int main()
{
    ensureConsole();
    galleryLog("==== AYAudio Gallery ====");
    galleryLog("How-to:");
    galleryLog("  1) Click [Play 440 Hz] — must hear a beep (device OK).");
    galleryLog("  2) [Open Stream…] pick .wav/.mp3/.ogg");
    galleryLog("  3) Raise Master / Music if quiet; ensure not Pause.");
    galleryLog("  Tip: paths with only ASCII are safest (CreateFileA).");

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"AYAudioGalleryWnd";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    GalleryState state;
    state.hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"AYAudio Gallery",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, kWindowW, kWindowH, nullptr, nullptr,
        inst, nullptr);
    if (!state.hwnd)
    {
        std::fprintf(stderr, "[AYAudioGallery] CreateWindow failed\n");
        return 1;
    }
    SetWindowLongPtrW(state.hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(&state));
    createUi(state, inst);
    ShowWindow(state.hwnd, SW_SHOW);
    UpdateWindow(state.hwnd);

    if (!initAudio(state))
    {
        MessageBoxW(state.hwnd,
                    L"Failed to initialize AYAudio (Miniaudio). "
                    L"Check that a playback device is available.",
                    L"AYAudio Gallery", MB_ICONERROR | MB_OK);
    }

    SetTimer(state.hwnd, kTimerId, 16, nullptr);

    MSG msg{};
    while (state.running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                state.running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!state.running)
        {
            break;
        }
        // Keep command pump tight even if timer coalesces.
        state.audio.update(1.0f / 60.0f);
        Sleep(1);
    }

    KillTimer(state.hwnd, kTimerId);
    if (state.stream.voice != ayt::audio::InvalidVoice)
    {
        state.audio.stopStreaming(state.stream);
        state.stream = {};
    }
    state.audio.shutdown();
    return 0;
}
