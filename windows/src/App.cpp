// CapView for Windows: window, menus, shortcuts, device selection and the frame watchdog.
#include "App.h"

#include "Clipboard.h"
#include "resource.h"

#include <dbt.h>
#include <imm.h>
#include <mferror.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace {

constexpr wchar_t kWindowClass[] = L"CapViewWindow";

enum TimerId : UINT_PTR { kTimerWatchdog = 1, kTimerRefresh, kTimerIdle, kTimerVideoRetry };

// Let the display sleep only after this long without any picture: a console blanks HDMI
// for a few seconds whenever it changes video mode.
constexpr int kNoSignalSleepDelay = 300;
constexpr ULONGLONG kCursorIdleMs = 2000;

enum CommandId : UINT {
    kCmdSnapshot = 100,
    kCmdReconnect,
    kCmdResolutionAuto,
    kCmdFrameRateAuto,
    kCmdAudioAuto,
    kCmdAudioOff,
    kCmdMute,
    kCmdVolumeUp,
    kCmdVolumeDown,
    kCmdFullScreen,
    kCmdTopMost,
    kCmdActualSize,
    kCmdPicture,
    kCmdCameraSettings,
    kCmdMicrophoneSettings,
    kCmdOpenLog,
    kCmdExit,
    kCmdVideoDeviceBase = 1000,
    kCmdResolutionBase = 2000,
    kCmdFrameRateBase = 3000,
    kCmdAudioDeviceBase = 4000,
};

std::wstring FormatRate(double fps) {
    return std::fabs(fps - std::round(fps)) < 0.005 ? std::format(L"{:.0f}", fps) : std::format(L"{:.2f}", fps);
}

void AppendItem(HMENU menu, UINT id, const std::wstring& text, bool checked = false, bool enabled = true) {
    UINT flags = MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED);
    AppendMenuW(menu, flags, id, text.c_str());
}

void AppendLabel(HMENU menu, const std::wstring& text) {
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, text.c_str());
}

void AppendSubmenu(HMENU menu, HMENU submenu, const std::wstring& text) {
    AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(submenu), text.c_str());
}

void OpenUri(const wchar_t* uri) {
    ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
}

/// Windows' privacy switches (Settings › Privacy & security › Camera / Microphone): the global
/// switch, the per-user switch and "let desktop apps access …". `capability` is "webcam" or
/// "microphone". Checked up front because a blocked camera otherwise just looks absent.
bool PrivacyDenied(const wchar_t* capability) {
    const std::wstring key =
        std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\") + capability;
    auto denied = [](HKEY root, const std::wstring& subKey) {
        wchar_t value[16]{};
        DWORD size = sizeof(value);
        if (RegGetValueW(root, subKey.c_str(), L"Value", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) {
            return false;
        }
        return _wcsicmp(value, L"Deny") == 0;
    };
    return denied(HKEY_LOCAL_MACHINE, key) || denied(HKEY_CURRENT_USER, key) ||
           denied(HKEY_CURRENT_USER, key + L"\\NonPackaged");
}

constexpr wchar_t kCameraPrivacyMessage[] =
    L"캡처보드 화면을 보려면 카메라 권한이 필요합니다.\n"
    L"설정 › 개인 정보 및 보안 › 카메라에서\n"
    L"'카메라 액세스'와 '데스크톱 앱이 카메라에 액세스하도록 허용'을 켜 주세요.\n"
    L"(오른쪽 클릭 › 카메라 개인 정보 설정 열기)";

constexpr wchar_t kNoOutputMessage[] = L"소리를 낼 출력 장치가 없습니다";

}  // namespace

// MARK: - Lifecycle

int App::Run(HINSTANCE instance, int showCommand) {
    instance_ = instance;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // Created directly at the saved rectangle (if its monitor is still there): moving it there
    // afterwards would cross monitors and make Windows rescale it for the new DPI every launch.
    auto savedRect = Settings::WindowRect();
    if (savedRect && !MonitorFromRect(&*savedRect, MONITOR_DEFAULTTONULL)) savedRect.reset();
    RECT initial = savedRect.value_or(RECT{CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT + 1280, CW_USEDEFAULT + 720});
    hwnd_ = CreateWindowExW(0, kWindowClass, L"CapView", WS_OVERLAPPEDWINDOW, initial.left, initial.top,
                            initial.right - initial.left, initial.bottom - initial.top, nullptr, nullptr, instance, this);
    if (!hwnd_) {
        Log(L"CreateWindowExW failed (%lu)", GetLastError());
        return 1;
    }

    // Single-key shortcuts (F, M) must not be swallowed by the Korean IME.
    ImmAssociateContextEx(hwnd_, nullptr, 0);

    renderer_ = std::make_unique<Renderer>();
    if (FAILED(LogIfFailed(renderer_->Initialize(hwnd_), L"Renderer::Initialize"))) {
        MessageBoxW(hwnd_, L"그래픽 장치를 초기화하지 못했습니다. 그래픽 드라이버를 업데이트해 주세요.", L"CapView", MB_ICONERROR);
        return 1;
    }
    Renderer* renderer = renderer_.get();
    video_ = std::make_unique<VideoCapture>(hwnd_, [renderer](IMFSample* sample, const VideoMode& mode) {
        renderer->SubmitFrame(sample, mode.width, mode.height, mode.subtype);
    });
    audio_ = std::make_unique<AudioPassthrough>(hwnd_);
    audioWatcher_ = std::make_unique<AudioDeviceWatcher>(hwnd_);

    volume_ = Settings::Volume();
    muted_ = Settings::Muted();
    audio_->SetVolume(volume_);
    audio_->SetMuted(muted_);
    picture_ = Settings::Picture();
    renderer_->SetPicture(picture_);
    renderer_->SetMessage(L"캡처보드를 준비하는 중…");

    // First launch: a 1280x720 picture centred on screen.
    if (!savedRect) {
        SetClientSize(1280, 720);
        RECT window{};
        GetWindowRect(hwnd_, &window);
        HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(monitor, &info);
        int w = window.right - window.left, h = window.bottom - window.top;
        SetWindowPos(hwnd_, nullptr, info.rcWork.left + (info.rcWork.right - info.rcWork.left - w) / 2,
                     info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - h) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    FitWindowToAspect();
    if (Settings::TopMost()) ToggleTopMost();

    ShowWindow(hwnd_, showCommand);
    UpdateWindow(hwnd_);

    SetTimer(hwnd_, kTimerWatchdog, 1000, nullptr);
    SetTimer(hwnd_, kTimerIdle, 500, nullptr);
    lastFrameCheck_ = GetTickCount64();
    RefreshDevices();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (pictureDialog_.PreTranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* app = static_cast<App*>(create->lpCreateParams);
        app->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return app ? app->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (renderer_) {
            renderer_->SetVisible(wParam != SIZE_MINIMIZED);
            if (wParam != SIZE_MINIMIZED) renderer_->Resize(LOWORD(lParam), HIWORD(lParam));
        }
        UpdateSleepPrevention();
        return 0;

    case WM_SIZING:
        if (!fullScreen_) ConstrainSizing(wParam, reinterpret_cast<RECT*>(lParam));
        return TRUE;

    case WM_DPICHANGED: {
        if (fullScreen_) {
            MONITORINFO info{sizeof(info)};
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info);
            SetWindowPos(hwnd_, nullptr, info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
                         info.rcMonitor.bottom - info.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        auto* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;  // the swap chain covers the client area

    case WM_LBUTTONDBLCLK:
        ToggleFullScreen();
        return 0;

    case WM_CONTEXTMENU: {
        POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        if (point.x == -1 && point.y == -1) {  // keyboard menu key: open at the window centre
            RECT client{};
            GetClientRect(hwnd_, &client);
            point = {client.right / 2, client.bottom / 2};
            ClientToScreen(hwnd_, &point);
        } else {
            // Right-clicks on the title bar keep the standard window menu.
            POINT inClient = point;
            ScreenToClient(hwnd_, &inClient);
            RECT client{};
            GetClientRect(hwnd_, &client);
            if (!PtInRect(&client, inClient)) break;
        }
        ShowContextMenu(point);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (HandleKey(wParam, lParam)) return 0;
        break;

    case WM_MOUSEMOVE: {
        POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        if (point.x != lastMouse_.x || point.y != lastMouse_.y) {  // ignore synthetic repeats
            lastMouse_ = point;
            lastMouseMove_ = GetTickCount64();
            if (cursorHidden_) {
                cursorHidden_ = false;
                UpdateCursor();
            }
        }
        break;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && cursorHidden_) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;

    case WM_TIMER:
        switch (wParam) {
        case kTimerWatchdog: CheckFrames(); break;
        case kTimerRefresh:
            KillTimer(hwnd_, kTimerRefresh);
            RefreshDevices();
            break;
        case kTimerVideoRetry:
            KillTimer(hwnd_, kTimerVideoRetry);
            if (activeVideo_) video_->Restart();
            break;
        case kTimerIdle: {
            if (cursorHidden_ || menuOpen_ || GetForegroundWindow() != hwnd_) break;
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(hwnd_, &cursor);
            RECT client{};
            GetClientRect(hwnd_, &client);
            if (PtInRect(&client, cursor) && GetTickCount64() - lastMouseMove_ >= kCursorIdleMs) {
                cursorHidden_ = true;
                UpdateCursor();
            }
            break;
        }
        }
        return 0;

    case WM_SYSCOMMAND:
        // ES_DISPLAY_REQUIRED does not stop the screen saver; refuse it while a game is on screen.
        if (sleepPrevented_ && ((wParam & 0xFFF0) == SC_SCREENSAVE || (wParam & 0xFFF0) == SC_MONITORPOWER)) return 0;
        break;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVNODES_CHANGED || wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
            ScheduleRefresh();
        }
        return TRUE;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC) {
            Log(L"Resumed from sleep; reconnecting");
            Reconnect();
        }
        return TRUE;

    case WM_APP_VIDEO_STARTED:
        OnVideoStarted(static_cast<UINT>(wParam), static_cast<HRESULT>(lParam));
        return 0;

    case WM_APP_VIDEO_RESTARTED:
        OnVideoRestarted(static_cast<UINT>(wParam), static_cast<HRESULT>(lParam));
        return 0;

    case WM_APP_VIDEO_MODE_CHANGED:
        OnVideoModeChanged(static_cast<UINT>(wParam));
        return 0;

    case WM_ACTIVATEAPP:
        // Back from Settings after allowing camera or microphone access: try again at once.
        if (wParam && (cameraDenied_ || microphoneDenied_)) ScheduleRefresh();
        break;

    case WM_APP_VIDEO_ERROR:
        if (static_cast<UINT>(wParam) == videoGeneration_ && activeVideo_) {
            Log(L"Video stream error 0x%08X; retrying in 1 s", static_cast<unsigned>(lParam));
            SetTimer(hwnd_, kTimerVideoRetry, 1000, nullptr);
        }
        return 0;

    case WM_APP_SNAPSHOT: {
        std::unique_ptr<SnapshotImage> image(reinterpret_cast<SnapshotImage*>(lParam));
        if (!image || image->bgra.empty()) {
            renderer_->ShowToast(L"캡처할 화면이 없습니다");
        } else if (CopyImageToClipboard(hwnd_, *image)) {
            renderer_->ShowToast(L"화면을 클립보드에 복사했습니다");
        } else {
            renderer_->ShowToast(L"클립보드에 복사하지 못했습니다");
        }
        return 0;
    }

    case WM_APP_AUDIO_FAILED: {
        std::unique_ptr<std::wstring> reason(reinterpret_cast<std::wstring*>(lParam));
        audioFailure_ = reason ? *reason : L"오디오가 멈췄습니다";
        Log(L"Audio stopped: %ls", audioFailure_.c_str());
        return 0;  // the watchdog retries
    }

    case WM_APP_AUDIO_DEVICES_CHANGED:
        ScheduleRefresh();
        return 0;

    case WM_APP_DEFAULT_OUTPUT_CHANGED:
        if (audio_) audio_->OnDefaultOutputChanged();
        return 0;

    case WM_CLOSE:
        if (!fullScreen_ && !IsIconic(hwnd_) && !IsZoomed(hwnd_)) {
            RECT rect{};
            GetWindowRect(hwnd_, &rect);
            Settings::SetWindowRect(rect);
        }
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd_, kTimerWatchdog);
        KillTimer(hwnd_, kTimerIdle);
        pictureDialog_.Close();
        if (audio_) audio_->Stop();
        if (video_) video_->Stop();
        audioWatcher_.reset();
        audio_.reset();
        video_.reset();
        if (renderer_) renderer_->Shutdown();
        renderer_.reset();
        SetThreadExecutionState(ES_CONTINUOUS);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

// MARK: - Devices

void App::ScheduleRefresh() {
    // Plugging in a card fires several notifications (video, audio, devnodes); handle them once.
    SetTimer(hwnd_, kTimerRefresh, 300, nullptr);
}

void App::RefreshDevices() {
    videoDevices_ = EnumerateVideoDevices();
    audioInputs_ = EnumerateAudioInputs();
    microphoneDenied_ = PrivacyDenied(L"microphone");

    if (PrivacyDenied(L"webcam")) {
        cameraDenied_ = true;
        StopVideo(kCameraPrivacyMessage);
    } else if (auto wanted = PreferredVideoDevice()) {
        bool sameDevice = activeVideo_ && activeVideo_->symbolicLink == wanted->symbolicLink;
        if (!sameDevice || videoFailed_) StartVideo(*wanted);
    } else if (!Settings::VideoName().empty()) {
        StopVideo(std::format(L"“{}”을(를) 찾을 수 없습니다.\nUSB 연결을 확인하거나, 오른쪽 클릭 › 비디오에서 다른 장치를 선택해 주세요.",
                              Settings::VideoName()));
    } else {
        StopVideo(L"캡처보드를 찾을 수 없습니다.\nUSB 연결을 확인하거나, 오른쪽 클릭 › 비디오에서 장치를 선택해 주세요.");
    }
    UpdateAudio();
}

std::optional<VideoDeviceInfo> App::PreferredVideoDevice() const {
    std::wstring link = Settings::VideoDeviceLink(), model = Settings::VideoModel(), name = Settings::VideoName();
    if (!link.empty() || !model.empty() || !name.empty()) {
        // The device used last time, wherever it is plugged in. Never another camera: a webcam's
        // microphone would end up on the speakers.
        for (const auto& d : videoDevices_) if (!link.empty() && d.symbolicLink == link) return d;
        for (const auto& d : videoDevices_) if (!model.empty() && d.model == model && d.name == name) return d;
        for (const auto& d : videoDevices_) if (!model.empty() && d.model == model) return d;
        for (const auto& d : videoDevices_) if (!name.empty() && !d.isVirtual && d.name == name) return d;
        return std::nullopt;
    }
    // First use: something that looks like a capture card, preferably with its own audio.
    for (const auto& d : videoDevices_) {
        if (!d.isVirtual && d.looksLikeCaptureCard && PairAudioInput(d, audioInputs_)) return d;
    }
    for (const auto& d : videoDevices_) {
        if (!d.isVirtual && d.looksLikeCaptureCard) return d;
    }
    return std::nullopt;
}

std::optional<AudioInputInfo> App::PreferredAudioInput() const {
    AudioSelection selection = Settings::Audio();
    if (selection.mode == AudioMode::Off) return std::nullopt;
    if (selection.mode == AudioMode::Device) {
        for (const auto& input : audioInputs_) if (input.id == selection.deviceId) return input;
    }
    if (!activeVideo_) return std::nullopt;
    if (auto index = PairAudioInput(*activeVideo_, audioInputs_)) return audioInputs_[*index];
    return std::nullopt;
}

void App::StartVideo(const VideoDeviceInfo& device) {
    activeVideo_ = device;
    activeMode_.reset();
    videoFailed_ = false;
    ResetFrameWatch();
    capabilities_ = {};  // filled from the opened device once the stream has started
    SetWindowTextW(hwnd_, device.name.c_str());
    renderer_->SetMessage(L"");
    Log(L"Starting video: %ls", device.name.c_str());
    videoGeneration_ = video_->Start(device.symbolicLink, Settings::Resolution(), Settings::FrameRate());
}

void App::OnVideoStarted(UINT generation, HRESULT hr) {
    if (generation != videoGeneration_ || !activeVideo_) return;
    if (SUCCEEDED(hr)) {
        activeMode_ = video_->Mode();
        capabilities_ = video_->Caps();
        cameraDenied_ = false;
        Settings::SetVideoDevice(activeVideo_->symbolicLink, activeVideo_->model, activeVideo_->name);
        Log(L"Video running: %ux%u @ %.3f", activeMode_->width, activeMode_->height, activeMode_->fps);
        renderer_->SetMessage(L"");
        FitWindowToAspect();
    } else {
        activeMode_.reset();
        videoFailed_ = true;
        video_->Stop();
        renderer_->ClearFrame();
        if (hr == E_ACCESSDENIED) {
            cameraDenied_ = true;
            renderer_->SetMessage(kCameraPrivacyMessage);
        } else {
            renderer_->SetMessage(std::format(L"“{}”을(를) 시작하지 못했습니다. (0x{:08X})\n"
                                              L"다른 앱이 쓰고 있지 않은지 확인하고, 오른쪽 클릭 › 비디오 › 다시 연결을 눌러 주세요.",
                                              activeVideo_->name, static_cast<unsigned>(hr)));
        }
    }
    UpdateSleepPrevention();
}

/// The watchdog's Restart() finished. On failure the watchdog keeps trying; this only says why
/// the picture is gone when "no signal" would send the user to the wrong place.
void App::OnVideoRestarted(UINT generation, HRESULT hr) {
    if (generation != videoGeneration_ || !activeVideo_ || !activeMode_) return;
    if (SUCCEEDED(hr)) {
        videoFailed_ = false;
        cameraDenied_ = false;
        capabilities_ = video_->Caps();
        if (showingNoSignal_) {
            showingNoSignal_ = false;  // the watchdog shows "no signal" again if frames still do not come
            renderer_->SetMessage(L"");
        }
        OnVideoModeChanged(generation);  // a restart may land on another format
        return;
    }
    videoFailed_ = true;  // a device change or coming back to the window starts it from scratch
    std::wstring message;
    if (hr == E_ACCESSDENIED) {
        cameraDenied_ = true;
        message = kCameraPrivacyMessage;
    } else if (hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED || hr == MF_E_VIDEO_DEVICE_LOCKED ||
               hr == MF_E_HW_MFT_FAILED_START_STREAMING || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
               hr == HRESULT_FROM_WIN32(ERROR_BUSY)) {
        message = std::format(L"“{}”을(를) 다른 앱이 쓰고 있습니다.\nOBS 같은 캡처 앱을 닫으면 자동으로 다시 연결합니다.",
                              activeVideo_->name);
    } else {
        return;  // keep the no-signal text
    }
    showingNoSignal_ = true;  // the first frame clears it
    renderer_->SetMessage(message);
}

void App::OnVideoModeChanged(UINT generation) {
    if (generation != videoGeneration_ || !activeVideo_ || !activeMode_) return;
    const VideoMode mode = video_->Mode();
    if (mode.width == 0 || mode.height == 0) return;
    const bool resized = mode.width != activeMode_->width || mode.height != activeMode_->height;
    if (resized || mode.fps != activeMode_->fps) {
        Log(L"Video mode now %ux%u @ %.3f (was %ux%u @ %.3f)", mode.width, mode.height, mode.fps, activeMode_->width,
            activeMode_->height, activeMode_->fps);
    }
    activeMode_ = mode;
    if (resized) FitWindowToAspect();
}

void App::StopVideo(const std::wstring& message) {
    ++videoGeneration_;
    if (activeVideo_) video_->Stop();
    renderer_->ClearFrame();
    activeVideo_.reset();
    activeMode_.reset();
    videoFailed_ = false;
    capabilities_ = {};
    ResetFrameWatch();
    SetWindowTextW(hwnd_, L"CapView");
    renderer_->SetMessage(message);
    UpdateSleepPrevention();
}

/// Brings the running audio in line with the selection. The engine's own state is the
/// truth, so this also retries after a failed restart.
void App::UpdateAudio() {
    auto wanted = PreferredAudioInput();
    if ((wanted ? wanted->id : std::wstring()) == audio_->InputId()) return;
    bool hadFailure = !audioFailure_.empty();
    audio_->Stop();
    audioFailure_.clear();
    if (!wanted) return;
    HRESULT hr = audio_->Start(wanted->id);
    if (SUCCEEDED(hr)) {
        microphoneDenied_ = false;
        return;
    }
    microphoneDenied_ = microphoneDenied_ || hr == E_ACCESSDENIED;
    if (microphoneDenied_) {
        audioFailure_ = L"마이크 권한이 없어 소리를 재생할 수 없습니다";
    } else if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {  // no default output (E_NOTFOUND)
        audioFailure_ = kNoOutputMessage;
    } else if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
        audioFailure_ = L"오디오 장치가 응답하지 않습니다. 잠시 후 다시 시도합니다";
    } else {
        audioFailure_ = std::format(L"{}: 오디오를 시작하지 못했습니다 (0x{:08X})", wanted->name, static_cast<unsigned>(hr));
    }
    if (!hadFailure) renderer_->ShowToast(microphoneDenied_ ? L"마이크 권한이 없어 소리가 나지 않습니다" : L"오디오를 시작하지 못했습니다");
}

/// Every second: measures the frame rate, shows "no signal" when frames stop, and restarts
/// a stream that silently stopped delivering. Also retries audio every 5 s.
void App::CheckFrames() {
    // Retry audio every 5 s, backing off to a minute while it keeps failing (each attempt logs a
    // lot), except while there is simply no output device: plugging one in sends a device-change
    // notification, which retries right away.
    if (++watchdogTicks_ >= nextAudioRetry_ && audioFailure_ != kNoOutputMessage) {
        UpdateAudio();
        audioRetryDelay_ = audioFailure_.empty() ? 5 : std::min(audioRetryDelay_ * 2, 60);
        nextAudioRetry_ = watchdogTicks_ + audioRetryDelay_;
    }

    ULONGLONG now = GetTickCount64();
    uint64_t received = video_->FramesReceived();
    uint64_t newFrames = received - lastFrameCount_;
    measuredFps_ = static_cast<double>(newFrames) * 1000.0 / static_cast<double>(std::max<ULONGLONG>(now - lastFrameCheck_, 1));
    lastFrameCount_ = received;
    lastFrameCheck_ = now;
    if (auto status = audio_->Status(); status && status->inputGaps != lastInputGaps_) {
        lastInputGaps_ = status->inputGaps;
        lastInputGapTime_ = now;
    }

    if (!activeVideo_ || !activeMode_) {
        secondsWithoutFrames_ = 0;
        return;
    }
    if (newFrames > 0) {
        if (showingNoSignal_) {
            showingNoSignal_ = false;
            renderer_->SetMessage(L"");
        }
        bool wasAsleep = secondsWithoutFrames_ >= kNoSignalSleepDelay;
        secondsWithoutFrames_ = 0;
        if (wasAsleep) UpdateSleepPrevention();
        return;
    }
    ++secondsWithoutFrames_;
    if (secondsWithoutFrames_ == kNoSignalSleepDelay) UpdateSleepPrevention();
    if (secondsWithoutFrames_ >= 2 && !showingNoSignal_) {
        showingNoSignal_ = true;
        renderer_->SetMessage(std::format(L"“{}”에서 영상이 들어오지 않습니다.\n게임기 전원과 HDMI 연결을 확인해 주세요.\n"
                                          L"PlayStation은 설정에서 HDCP를 꺼야 캡처됩니다.",
                                          activeVideo_->name));
    }
    // A capture stream can stop delivering without any error.
    int s = secondsWithoutFrames_;
    if (s == 3 || s == 10 || s == 30 || s % 60 == 0) video_->Restart();
}

void App::ResetFrameWatch() {
    secondsWithoutFrames_ = 0;
    showingNoSignal_ = false;
    if (video_) lastFrameCount_ = video_->FramesReceived();
}

void App::UpdateSleepPrevention() {
    bool picture = activeMode_ && secondsWithoutFrames_ < kNoSignalSleepDelay;
    bool visible = hwnd_ && !IsIconic(hwnd_);
    bool prevent = picture && visible;
    if (prevent == sleepPrevented_) return;
    sleepPrevented_ = prevent;
    // A game controller generates no PC input, so without this the display would sleep mid-game.
    SetThreadExecutionState(prevent ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS);
}

// MARK: - Actions

void App::Reconnect() {
    audio_->Stop();
    audioFailure_.clear();
    StopVideo(L"다시 연결하는 중…");
    RefreshDevices();
}

void App::ToggleMute() {
    muted_ = !muted_;
    Settings::SetMuted(muted_);
    audio_->SetMuted(muted_);
    renderer_->ShowToast(muted_ ? L"음소거" : std::format(L"소리 켬 {}%", static_cast<int>(std::lround(volume_ * 100))));
}

void App::ChangeVolume(float delta) {
    volume_ = std::clamp(std::round((volume_ + delta) * 10.0f) / 10.0f, 0.0f, 1.0f);
    muted_ = false;
    Settings::SetVolume(volume_);
    Settings::SetMuted(false);
    audio_->SetVolume(volume_);
    audio_->SetMuted(false);
    renderer_->ShowToast(std::format(L"볼륨 {}%", static_cast<int>(std::lround(volume_ * 100))));
}

void App::ToggleFullScreen() {
    if (!fullScreen_) {
        savedPlacement_.length = sizeof(savedPlacement_);
        GetWindowPlacement(hwnd_, &savedPlacement_);
        savedStyle_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (savedStyle_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        fullScreen_ = true;
        SetWindowPos(hwnd_, topMost_ ? HWND_TOPMOST : HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        fullScreen_ = false;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, savedStyle_);
        SetWindowPlacement(hwnd_, &savedPlacement_);
        SetWindowPos(hwnd_, topMost_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        FitWindowToAspect();
    }
}

void App::ToggleTopMost() {
    topMost_ = !topMost_;
    Settings::SetTopMost(topMost_);
    SetWindowPos(hwnd_, topMost_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void App::ActualSize() {
    if (fullScreen_ || !activeMode_) return;
    if (IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info);
    RECT insets = NonClientInsets();
    double maxW = info.rcWork.right - info.rcWork.left - (insets.left + insets.right);
    double maxH = info.rcWork.bottom - info.rcWork.top - (insets.top + insets.bottom);
    double fit = std::min({1.0, maxW / activeMode_->width, maxH / activeMode_->height});
    SetClientSize(static_cast<int>(std::lround(activeMode_->width * fit)), static_cast<int>(std::lround(activeMode_->height * fit)));
    FitWindowToAspect();  // also moves it back on screen
}

void App::ShowPictureDialog() {
    pictureDialog_.Show(hwnd_, picture_, [this](const PictureSettings& settings) {
        picture_ = settings;
        Settings::SetPicture(settings);
        renderer_->SetPicture(settings);
    });
}

void App::CopySnapshot() {
    renderer_->RequestSnapshot(hwnd_);
}

/// Single-key shortcuts. Returns true when handled.
bool App::HandleKey(WPARAM key, LPARAM flags) {
    bool repeat = (flags & (1 << 30)) != 0;
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    bool alt = GetKeyState(VK_MENU) < 0;
    if (alt) return false;
    if (ctrl) {
        if (repeat) return key == 'C' || key == 'E' || key == 'T' || key == '0' || key == 'R';
        switch (key) {
        case 'C': CopySnapshot(); return true;
        case 'E': ShowPictureDialog(); return true;
        case 'T':
            ToggleTopMost();
            renderer_->ShowToast(topMost_ ? L"항상 위에 표시" : L"항상 위에 표시 끔");
            return true;
        case '0': ActualSize(); return true;
        case 'R': Reconnect(); return true;
        }
        return false;
    }
    switch (key) {
    case VK_UP: ChangeVolume(0.1f); return true;
    case VK_DOWN: ChangeVolume(-0.1f); return true;
    case 'F': if (!repeat) ToggleFullScreen(); return true;
    case 'M': if (!repeat) ToggleMute(); return true;
    case VK_ESCAPE:
        if (fullScreen_ && !repeat) ToggleFullScreen();
        return fullScreen_;
    }
    return false;
}

// MARK: - Context menu

void App::ShowContextMenu(POINT screenPoint) {
    HMENU menu = CreatePopupMenu();
    menuVideoLinks_.clear();
    menuSizes_.clear();
    menuRates_.clear();
    menuAudioIds_.clear();

    AppendItem(menu, kCmdSnapshot, L"화면 캡처 (클립보드에 복사)\tCtrl+C");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // 비디오
    HMENU videoMenu = CreatePopupMenu();
    if (activeVideo_ && activeMode_) {
        AppendLabel(videoMenu, std::format(L"{} — {}×{} @ {}", activeVideo_->name, activeMode_->width,
                                           activeMode_->height, FormatRate(activeMode_->fps)));
        AppendLabel(videoMenu, std::format(L"측정: {:.1f} fps", measuredFps_));
        AppendMenuW(videoMenu, MF_SEPARATOR, 0, nullptr);
    }
    AppendLabel(videoMenu, L"캡처 장치");
    if (videoDevices_.empty()) AppendLabel(videoMenu, L"연결된 장치 없음");
    for (const auto& device : videoDevices_) {
        UINT id = kCmdVideoDeviceBase + static_cast<UINT>(menuVideoLinks_.size());
        menuVideoLinks_.push_back(device.symbolicLink);
        bool checked = activeVideo_ && activeVideo_->symbolicLink == device.symbolicLink;
        AppendItem(videoMenu, id, device.isVirtual ? device.name + L" (가상)" : device.name, checked);
    }
    AppendMenuW(videoMenu, MF_SEPARATOR, 0, nullptr);

    auto savedSize = Settings::Resolution();
    HMENU sizeMenu = CreatePopupMenu();
    AppendItem(sizeMenu, kCmdResolutionAuto, L"자동 (60 fps 우선, 최대 해상도)", !savedSize);
    for (const SIZE& size : capabilities_.sizes) {
        UINT id = kCmdResolutionBase + static_cast<UINT>(menuSizes_.size());
        menuSizes_.push_back(size);
        bool checked = savedSize && savedSize->cx == size.cx && savedSize->cy == size.cy;
        AppendItem(sizeMenu, id, std::format(L"{} × {}", size.cx, size.cy), checked);
    }
    AppendSubmenu(videoMenu, sizeMenu, L"해상도");

    auto savedRate = Settings::FrameRate();
    HMENU rateMenu = CreatePopupMenu();
    AppendItem(rateMenu, kCmdFrameRateAuto, L"자동 (최대)", !savedRate);
    UINT32 rw = activeMode_ ? activeMode_->width : (capabilities_.sizes.empty() ? 0 : capabilities_.sizes[0].cx);
    UINT32 rh = activeMode_ ? activeMode_->height : (capabilities_.sizes.empty() ? 0 : capabilities_.sizes[0].cy);
    for (double rate : capabilities_.RatesFor(rw, rh)) {
        UINT id = kCmdFrameRateBase + static_cast<UINT>(menuRates_.size());
        menuRates_.push_back(rate);
        bool checked = savedRate && std::fabs(*savedRate - rate) < 0.005;
        AppendItem(rateMenu, id, FormatRate(rate) + L" fps", checked);
    }
    AppendSubmenu(videoMenu, rateMenu, L"프레임레이트");
    AppendMenuW(videoMenu, MF_SEPARATOR, 0, nullptr);
    AppendItem(videoMenu, kCmdReconnect, L"다시 연결\tCtrl+R");
    AppendSubmenu(menu, videoMenu, L"비디오");

    // 오디오
    HMENU audioMenu = CreatePopupMenu();
    if (microphoneDenied_) {
        AppendLabel(audioMenu, L"마이크 권한이 없어 소리를 재생할 수 없습니다");
        AppendItem(audioMenu, kCmdMicrophoneSettings, L"마이크 개인 정보 설정 열기…");
        AppendMenuW(audioMenu, MF_SEPARATOR, 0, nullptr);
    }
    AudioSelection selection = Settings::Audio();
    std::wstring paired = L"캡처보드 오디오 없음";
    if (activeVideo_) {
        if (auto index = PairAudioInput(*activeVideo_, audioInputs_)) paired = audioInputs_[*index].name;
    }
    AppendItem(audioMenu, kCmdAudioAuto, L"자동 — " + paired, selection.mode == AudioMode::Automatic);
    AppendItem(audioMenu, kCmdAudioOff, L"끄기", selection.mode == AudioMode::Off);
    AppendMenuW(audioMenu, MF_SEPARATOR, 0, nullptr);
    AppendLabel(audioMenu, L"입력 장치");
    for (const auto& input : audioInputs_) {
        UINT id = kCmdAudioDeviceBase + static_cast<UINT>(menuAudioIds_.size());
        menuAudioIds_.push_back(input.id);
        AppendItem(audioMenu, id, input.name, selection.mode == AudioMode::Device && selection.deviceId == input.id);
    }
    if (!audioFailure_.empty()) AppendLabel(audioMenu, L"⚠ " + audioFailure_);
    if (auto status = audio_->Status()) {
        AppendLabel(audioMenu, std::format(L"예상 오디오 지연 ≈ {:.0f} ms", status->latencyMs));
        if (status->inputGaps > 0 || status->underruns > 0) {
            AppendLabel(audioMenu, std::format(L"소리 끊김: 캡처보드 {}회 · 재생 {}회", status->inputGaps, status->underruns));
        }
        if (lastInputGapTime_ != 0 && GetTickCount64() - lastInputGapTime_ < 300000) {
            AppendLabel(audioMenu, L"캡처보드의 USB 오디오가 끊기고 있습니다. 허브 대신 PC에 직접 연결해 보세요.");
        }
    }
    AppendMenuW(audioMenu, MF_SEPARATOR, 0, nullptr);
    AppendItem(audioMenu, kCmdMute, L"음소거\tM", muted_);
    AppendItem(audioMenu, kCmdVolumeUp, L"볼륨 크게\t↑");
    AppendItem(audioMenu, kCmdVolumeDown, L"볼륨 작게\t↓");
    AppendLabel(audioMenu, std::format(L"볼륨: {}%", static_cast<int>(std::lround(volume_ * 100))));
    AppendSubmenu(menu, audioMenu, L"오디오");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendItem(menu, kCmdFullScreen, fullScreen_ ? L"전체 화면 종료\tF" : L"전체 화면\tF");
    AppendItem(menu, kCmdTopMost, L"항상 위에 표시\tCtrl+T", topMost_);
    AppendItem(menu, kCmdActualSize, L"실제 픽셀 크기\tCtrl+0", false, !fullScreen_ && activeMode_.has_value());
    AppendItem(menu, kCmdPicture, L"화면 조정…\tCtrl+E");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (cameraDenied_) AppendItem(menu, kCmdCameraSettings, L"카메라 개인 정보 설정 열기…");
    AppendItem(menu, kCmdOpenLog, L"로그 폴더 열기");
    AppendItem(menu, kCmdExit, L"종료");

    menuOpen_ = true;
    if (cursorHidden_) {
        cursorHidden_ = false;
        UpdateCursor();
    }
    SetForegroundWindow(hwnd_);
    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    menuOpen_ = false;
    lastMouseMove_ = GetTickCount64();
    DestroyMenu(menu);  // also destroys the submenus
    if (command) HandleCommand(command);
}

void App::HandleCommand(UINT id) {
    if (id >= kCmdVideoDeviceBase && id < kCmdVideoDeviceBase + menuVideoLinks_.size()) {
        const std::wstring& link = menuVideoLinks_[id - kCmdVideoDeviceBase];
        for (const auto& device : videoDevices_) {
            if (device.symbolicLink == link) {
                Settings::SetVideoDevice(device.symbolicLink, device.model, device.name);
                break;
            }
        }
        RefreshDevices();
        return;
    }
    if (id >= kCmdResolutionBase && id < kCmdResolutionBase + menuSizes_.size()) {
        Settings::SetResolution(menuSizes_[id - kCmdResolutionBase]);
        if (activeVideo_) StartVideo(*activeVideo_);
        return;
    }
    if (id >= kCmdFrameRateBase && id < kCmdFrameRateBase + menuRates_.size()) {
        Settings::SetFrameRate(menuRates_[id - kCmdFrameRateBase]);
        if (activeVideo_) StartVideo(*activeVideo_);
        return;
    }
    if (id >= kCmdAudioDeviceBase && id < kCmdAudioDeviceBase + menuAudioIds_.size()) {
        Settings::SetAudio({AudioMode::Device, menuAudioIds_[id - kCmdAudioDeviceBase]});
        UpdateAudio();
        return;
    }
    switch (id) {
    case kCmdSnapshot: CopySnapshot(); break;
    case kCmdReconnect: Reconnect(); break;
    case kCmdResolutionAuto:
        Settings::SetResolution(std::nullopt);
        if (activeVideo_) StartVideo(*activeVideo_);
        break;
    case kCmdFrameRateAuto:
        Settings::SetFrameRate(std::nullopt);
        if (activeVideo_) StartVideo(*activeVideo_);
        break;
    case kCmdAudioAuto:
        Settings::SetAudio({AudioMode::Automatic, {}});
        UpdateAudio();
        break;
    case kCmdAudioOff:
        Settings::SetAudio({AudioMode::Off, {}});
        UpdateAudio();
        break;
    case kCmdMute: ToggleMute(); break;
    case kCmdVolumeUp: ChangeVolume(0.1f); break;
    case kCmdVolumeDown: ChangeVolume(-0.1f); break;
    case kCmdFullScreen: ToggleFullScreen(); break;
    case kCmdTopMost:
        ToggleTopMost();
        renderer_->ShowToast(topMost_ ? L"항상 위에 표시" : L"항상 위에 표시 끔");
        break;
    case kCmdActualSize: ActualSize(); break;
    case kCmdPicture: ShowPictureDialog(); break;
    case kCmdCameraSettings: OpenUri(L"ms-settings:privacy-webcam"); break;
    case kCmdMicrophoneSettings: OpenUri(L"ms-settings:privacy-microphone"); break;
    case kCmdOpenLog: OpenUri(LogFolder().c_str()); break;
    case kCmdExit: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    }
}

// MARK: - Window geometry

double App::VideoAspect() const {
    if (activeMode_ && activeMode_->width && activeMode_->height) {
        return static_cast<double>(activeMode_->width) / activeMode_->height;
    }
    return 16.0 / 9.0;
}

/// Thickness of the frame around the client area for the window's current DPI and style.
RECT App::NonClientInsets() const {
    RECT rect{0, 0, 0, 0};
    UINT dpi = GetDpiForWindow(hwnd_);
    AdjustWindowRectExForDpi(&rect, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)), dpi);
    return {-rect.left, -rect.top, rect.right, rect.bottom};
}

void App::SetClientSize(int width, int height) {
    RECT insets = NonClientInsets();
    SetWindowPos(hwnd_, nullptr, 0, 0, width + insets.left + insets.right, height + insets.top + insets.bottom,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/// Gives the window the video's aspect ratio, keeping its width, and keeps it on screen.
void App::FitWindowToAspect() {
    if (fullScreen_ || IsZoomed(hwnd_) || IsIconic(hwnd_)) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    int width = client.right;
    if (width <= 0) return;
    int height = static_cast<int>(std::lround(width / VideoAspect()));
    if (std::abs(height - client.bottom) >= 1) SetClientSize(width, height);

    RECT window{};
    GetWindowRect(hwnd_, &window);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info);
    int x = std::max(info.rcWork.left, std::min(window.left, info.rcWork.right - (window.right - window.left)));
    int y = std::max(info.rcWork.top, std::min(window.top, info.rcWork.bottom - (window.bottom - window.top)));
    if (x != window.left || y != window.top) {
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/// WM_SIZING: keeps the client area at the video's aspect ratio while the user drags an edge,
/// leaving the opposite edges where they are.
void App::ConstrainSizing(WPARAM edge, RECT* rect) const {
    RECT insets = NonClientInsets();
    int ncW = insets.left + insets.right, ncH = insets.top + insets.bottom;
    double aspect = VideoAspect();
    int clientW = std::max(static_cast<int>(rect->right - rect->left) - ncW, 320);
    int clientH = std::max(static_cast<int>(rect->bottom - rect->top) - ncH, 180);
    if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
        clientW = static_cast<int>(std::lround(clientH * aspect));
    } else {
        clientH = static_cast<int>(std::lround(clientW / aspect));
    }
    bool fromLeft = edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
    bool fromTop = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
    if (fromLeft) rect->left = rect->right - (clientW + ncW); else rect->right = rect->left + clientW + ncW;
    if (fromTop) rect->top = rect->bottom - (clientH + ncH); else rect->bottom = rect->top + clientH + ncH;
}

void App::UpdateCursor() {
    POINT cursor{};
    GetCursorPos(&cursor);
    if (WindowFromPoint(cursor) == hwnd_) {
        SetCursor(cursorHidden_ ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
    }
}
