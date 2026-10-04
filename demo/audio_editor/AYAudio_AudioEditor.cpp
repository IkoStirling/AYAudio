// AYAudio_AudioEditor.cpp — standalone AYUI mixer / playback desk.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <AYProject/Project.h>
#include <windowsx.h>

#include "AudioEditorSession.h"

#include <AYAudio/AudioBackendFactory.h>
#include <AYAudio/AudioSubSystem.h>

#include "AYDevice/DeviceManager.h"
#include "AYRenderer.h"
#include "AYRenderer/RenderTypes.h"
#include "AYRenderer/UIRenderBackend.h"
#include "AYUI.h"
#include "AYUI/Theme.h"
#include "AYUI/UIManager.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <vector>

#pragma comment(lib, "comdlg32.lib")

namespace
{

constexpr int kWidth = 960;
constexpr int kHeight = 900;

bool fileExists(const std::string &path)
{
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

std::string resolveChromePath()
{
    wchar_t executable[32768]{};
    ::GetModuleFileNameW(nullptr, executable, 32768);
    const std::vector<std::string> candidates = {
        (std::filesystem::path(executable).parent_path() / "assets" / "audio_editor.ui.json").string(),
        "assets/audio_editor.ui.json",
        "AYRuntime/AYAudio/demo/audio_editor/assets/audio_editor.ui.json",
        "../AYRuntime/AYAudio/demo/audio_editor/assets/audio_editor.ui.json",
        "../../AYRuntime/AYAudio/demo/audio_editor/assets/audio_editor.ui.json",
    };
    for (const std::string &path : candidates)
    {
        if (fileExists(path))
        {
            return path;
        }
    }
    return candidates.front();
}

// ACP path for ayio::File / CreateFileA (matches Gallery narrowForIo policy).
std::string showOpenAudioDialog(HWND owner)
{
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "Audio (*.wav;*.mp3;*.ogg)\0*.wav;*.mp3;*.ogg\0"
                      "All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!::GetOpenFileNameA(&ofn))
    {
        return {};
    }
    return std::string(path);
}

struct AppState
{
    ayt::ui::UIManager *ui = nullptr;
    ayt::audio::AudioEditorSession *session = nullptr;
    ayt::render::Renderer *renderer = nullptr;
    ayt::render::UIRenderBackend *uiBackend = nullptr;
    ayt::device::DeviceManager *devices = nullptr;
    int clientW = kWidth;
    int clientH = kHeight;
    bool running = true;
};

static HCURSOR cursorForHint(ayt::ui::UiCursorHint hint)
{
    static const HCURSOR arrow = ::LoadCursorW(nullptr, IDC_ARROW);
    static const HCURSOR we = ::LoadCursorW(nullptr, IDC_SIZEWE);
    static const HCURSOR hand = ::LoadCursorW(nullptr, IDC_HAND);
    switch (hint)
    {
    case ayt::ui::UiCursorHint::SizeWe:
    case ayt::ui::UiCursorHint::SizeHorizontal:
        return we;
    case ayt::ui::UiCursorHint::Hand:
        return hand;
    default:
        return arrow;
    }
}

std::intptr_t handleMessage(AppState *state, unsigned msg, std::uintptr_t wParam, std::intptr_t lParam, bool &handled)
{
    handled = false;
    if (state == nullptr || state->ui == nullptr)
    {
        return 0;
    }
    ayt::ui::UIManager &ui = *state->ui;

    switch (msg)
    {
    case WM_SETCURSOR: {
        ::SetCursor(cursorForHint(ui.getCursorHint()));
        handled = true;
        return TRUE;
    }
    case WM_SIZE: {
        state->clientW = LOWORD(lParam);
        state->clientH = HIWORD(lParam);
        if (state->clientW < 32)
        {
            state->clientW = 32;
        }
        if (state->clientH < 32)
        {
            state->clientH = 32;
        }
        ui.setClientSize(static_cast<float>(state->clientW), static_cast<float>(state->clientH));
        if (state->renderer != nullptr)
        {
            state->renderer->resize(static_cast<uint32_t>(state->clientW), static_cast<uint32_t>(state->clientH));
        }
        if (state->uiBackend != nullptr)
        {
            state->uiBackend->setFramebufferSize(static_cast<uint16_t>(state->clientW),
                                                 static_cast<uint16_t>(state->clientH));
        }
        handled = true;
        return 0;
    }
    case WM_MOUSEMOVE: {
        const auto logical =
            ui.physicalToLogical({static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam))});
        if (state->session && state->session->handleMouseMove(logical.x, logical.y))
        {
            handled = true;
            return 0;
        }
        ui.onMouseMove(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)));
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (state->session &&
            state->session->handleMouseButton(ui.physicalToLogical({static_cast<float>(GET_X_LPARAM(lParam)),
                                                                    static_cast<float>(GET_Y_LPARAM(lParam))})
                                                  .x,
                                              ui.physicalToLogical({static_cast<float>(GET_X_LPARAM(lParam)),
                                                                    static_cast<float>(GET_Y_LPARAM(lParam))})
                                                  .y,
                                              0, true))
        {
            ::SetCapture(static_cast<HWND>(state->devices->window().getWindowHandle()));
            handled = true;
            return 0;
        }
        ui.onMouseButtonDown(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)), 0);
        handled = true;
        return 0;
    }
    case WM_LBUTTONUP: {
        if (state->session &&
            state->session->handleMouseButton(ui.physicalToLogical({static_cast<float>(GET_X_LPARAM(lParam)),
                                                                    static_cast<float>(GET_Y_LPARAM(lParam))})
                                                  .x,
                                              ui.physicalToLogical({static_cast<float>(GET_X_LPARAM(lParam)),
                                                                    static_cast<float>(GET_Y_LPARAM(lParam))})
                                                  .y,
                                              0, false))
        {
            ::ReleaseCapture();
            handled = true;
            return 0;
        }
        ui.onMouseButtonUp(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)), 0);
        handled = true;
        return 0;
    }
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        const bool down = msg == WM_MBUTTONDOWN;
        if (state->session &&
            state->session->handleMouseButton(ui.physicalToLogical({static_cast<float>(GET_X_LPARAM(lParam)),
                                                                    static_cast<float>(GET_Y_LPARAM(lParam))})
                                                  .x,
                                              ui.physicalToLogical({static_cast<float>(GET_X_LPARAM(lParam)),
                                                                    static_cast<float>(GET_Y_LPARAM(lParam))})
                                                  .y,
                                              2, down))
        {
            if (down)
                ::SetCapture(static_cast<HWND>(state->devices->window().getWindowHandle()));
            else
                ::ReleaseCapture();
            handled = true;
            return 0;
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        ui.onMouseButtonDown(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)), 1);
        handled = true;
        return 0;
    }
    case WM_RBUTTONUP: {
        ui.onMouseButtonUp(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)), 1);
        handled = true;
        return 0;
    }
    case WM_MOUSEWHEEL: {
        constexpr float kPixelsPerNotch = 40.0f;
        const short raw = static_cast<short>(HIWORD(wParam));
        const float deltaY = -(static_cast<float>(raw) / static_cast<float>(WHEEL_DELTA)) * kPixelsPerNotch;
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (state->devices != nullptr)
        {
            HWND hwnd = static_cast<HWND>(state->devices->window().getWindowHandle());
            if (hwnd != nullptr)
            {
                ::ScreenToClient(hwnd, &pt);
            }
        }
        ui.onMouseWheel(static_cast<float>(pt.x), static_cast<float>(pt.y), deltaY);
        handled = true;
        return 0;
    }
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
        if (state->session)
            state->session->onFocusLost();
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int native=static_cast<int>(wParam);
        if (native==VK_SHIFT) native=MapVirtualKeyW((lParam>>16)&0xff,MAPVK_VSC_TO_VK_EX)==VK_RSHIFT?VK_RSHIFT:VK_LSHIFT;
        if (native==VK_MENU) native=(lParam&(1<<24))?VK_RMENU:VK_LMENU;
        if (!state->session || !state->session->handleKey(native, true))
            ui.onKeyDown(static_cast<int>(wParam));
        handled = true;
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int native=static_cast<int>(wParam);
        if (native==VK_SHIFT) native=MapVirtualKeyW((lParam>>16)&0xff,MAPVK_VSC_TO_VK_EX)==VK_RSHIFT?VK_RSHIFT:VK_LSHIFT;
        if (native==VK_MENU) native=(lParam&(1<<24))?VK_RMENU:VK_LMENU;
        if (!state->session || !state->session->handleKey(native, false))
            ui.onKeyUp(static_cast<int>(wParam));
        handled = true;
        return 0;
    }
    default:
        break;
    }
    return 0;
}

std::string chooseRecordingAssets(HWND owner) {
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    const bool cleanup=SUCCEEDED(initialized);
    if(FAILED(initialized) && initialized!=RPC_E_CHANGED_MODE) return {};
    IFileOpenDialog* dialog=nullptr;std::string selected;
    if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) {
        DWORD flags=0;dialog->GetOptions(&flags);dialog->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"选择当前项目的资产目录（Assets）");
        if(SUCCEEDED(dialog->Show(owner))) {
            IShellItem* item=nullptr;
            if(SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path=nullptr;
                if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) {
                    const auto utf8=std::filesystem::path(path).u8string();selected.assign(utf8.begin(),utf8.end());CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if(cleanup) CoUninitialize();return selected;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    AllocConsole();
    FILE *dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);

    ayt::audio::AudioSubSystem audio;
    audio.setBackend(ayt::audio::makeMiniaudioBackend());
    if (!audio.initialize())
    {
        std::fprintf(stderr, "[AYAudio_AudioEditor] audio init failed\n");
        return 1;
    }

    ayt::device::DeviceManager devices;
    ayt::device::DeviceConfig cfg{};
    cfg.window.title = "AYAudio Editor";
    cfg.window.width = kWidth;
    cfg.window.height = kHeight;
    if (!devices.initialize(cfg))
    {
        std::fprintf(stderr, "[AYAudio_AudioEditor] DeviceManager init failed\n");
        audio.shutdown();
        return 1;
    }

    ayt::device::WindowManager &window = devices.window();
    HWND hwnd = static_cast<HWND>(window.getWindowHandle());
    if (hwnd == nullptr)
    {
        std::fprintf(stderr, "[AYAudio_AudioEditor] no HWND\n");
        devices.shutdown();
        audio.shutdown();
        return 1;
    }

    ayt::render::Renderer renderer;
    ayt::render::InitDesc init{};
    init.windowHandle = hwnd;
    init.width = kWidth;
    init.height = kHeight;
    init.vsync = true;
    init.msaa = 0;
    if (!renderer.initialize(init))
    {
        std::fprintf(stderr, "[AYAudio_AudioEditor] Renderer init failed\n");
        devices.shutdown();
        audio.shutdown();
        return 1;
    }

    ayt::render::UIRenderBackend uiBackend;
    if (!uiBackend.initialize(renderer))
    {
        std::fprintf(stderr, "[AYAudio_AudioEditor] UIRenderBackend init failed\n");
        renderer.shutdown();
        devices.shutdown();
        audio.shutdown();
        return 1;
    }
    uiBackend.setFramebufferSize(static_cast<uint16_t>(kWidth), static_cast<uint16_t>(kHeight));

    ayt::ui::UIManager ui;
    ui.initialize(&uiBackend);
    ayt::ui::ThemeManager::get().ensureDefaultThemes();
    ayt::ui::ThemeManager::get().setActiveTheme("dark");

    AppState state{};
    state.ui = &ui;
    state.renderer = &renderer;
    state.uiBackend = &uiBackend;
    state.devices = &devices;
    state.clientW = kWidth;
    state.clientH = kHeight;

    const std::string chromePath = resolveChromePath();
    if (!ui.loadLayout(chromePath))
    {
        std::fprintf(stderr, "[AYAudio_AudioEditor] failed to load chrome: %s\n", chromePath.c_str());
        ui.shutdown();
        uiBackend.shutdown();
        renderer.shutdown();
        devices.shutdown();
        audio.shutdown();
        return 1;
    }
    ui.setClientSize(static_cast<float>(kWidth), static_cast<float>(kHeight));

    ayt::audio::AudioEditorSession session;
    state.session = &session;
    session.setAudio(&audio);
    session.setAssetRoot(std::filesystem::path(chromePath).parent_path().string());
    session.setPathPicker([hwnd]() { return showOpenAudioDialog(hwnd); });
    session.setRecordingFolderPicker([hwnd]{return chooseRecordingAssets(hwnd);});
    if (!session.attach(ui))
    {
        std::fprintf(stderr, "[AYAudio_AudioEditor] session.attach failed\n");
        ui.shutdown();
        uiBackend.shutdown();
        renderer.shutdown();
        devices.shutdown();
        audio.shutdown();
        return 1;
    }

    window.setWindowCloseCallback([&state]() { state.running = false; });
    window.setWindowMessageCallback(
        [&state](unsigned msg, std::uintptr_t wParam, std::intptr_t lParam, bool &handled) -> std::intptr_t {
            return handleMessage(&state, msg, wParam, lParam, handled);
        });

    std::fprintf(stderr, "[AYAudio_AudioEditor] chrome=%s\n", chromePath.c_str());

    LARGE_INTEGER qpcFreq{};
    LARGE_INTEGER qpcPrev{};
    ::QueryPerformanceFrequency(&qpcFreq);
    ::QueryPerformanceCounter(&qpcPrev);

    while (state.running && window.isWindowValid())
    {
        devices.pollEvents();

        LARGE_INTEGER qpcNow{};
        ::QueryPerformanceCounter(&qpcNow);
        float dt = static_cast<float>(static_cast<double>(qpcNow.QuadPart - qpcPrev.QuadPart) /
                                      static_cast<double>(qpcFreq.QuadPart));
        qpcPrev = qpcNow;
        if (dt < 0.0f)
        {
            dt = 0.0f;
        }
        if (dt > 0.1f)
        {
            dt = 0.1f;
        }

        session.tick(dt);

        ayt::render::ClearDesc clear;
        clear.r = 0.10f;
        clear.g = 0.10f;
        clear.b = 0.12f;
        clear.a = 1.0f;
        renderer.beginCompositeFrame(clear, static_cast<uint16_t>(state.clientW), static_cast<uint16_t>(state.clientH));
        uiBackend.setFramebufferSize(static_cast<uint16_t>(state.clientW), static_cast<uint16_t>(state.clientH));
        ui.update(dt);
        ui.layout();
        ui.populateFrame();
        ui.flushFrame();
        renderer.endFrame();
    }

    session.detach();
    ui.shutdown();
    uiBackend.shutdown();
    renderer.shutdown();
    devices.shutdown();
    audio.shutdown();
    return 0;
}
