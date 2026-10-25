// Wires devices, capture, audio, window and menus together (the Windows AppController).
#pragma once

#include "AudioPassthrough.h"
#include "Devices.h"
#include "PictureDialog.h"
#include "Renderer.h"
#include "Settings.h"
#include "VideoCapture.h"

class App {
public:
    int Run(HINSTANCE instance, int showCommand);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    // Devices
    void ScheduleRefresh();
    void RefreshDevices();
    std::optional<VideoDeviceInfo> PreferredVideoDevice() const;
    std::optional<AudioInputInfo> PreferredAudioInput() const;
    void StartVideo(const VideoDeviceInfo& device);
    void StopVideo(const std::wstring& message);
    void UpdateAudio();
    void OnVideoStarted(UINT generation, HRESULT hr);
    void OnVideoRestarted(UINT generation, HRESULT hr);
    void OnVideoModeChanged(UINT generation);
    void CheckFrames();
    void ResetFrameWatch();
    void UpdateSleepPrevention();

    // Actions
    void Reconnect();
    void ToggleMute();
    void ChangeVolume(float delta);
    void ToggleFullScreen();
    void ToggleTopMost();
    void ActualSize();
    void ShowPictureDialog();
    void CopySnapshot();
    void ShowContextMenu(POINT screenPoint);
    void HandleCommand(UINT id);
    bool HandleKey(WPARAM key, LPARAM flags);

    // Window geometry
    double VideoAspect() const;
    void FitWindowToAspect();
    void ConstrainSizing(WPARAM edge, RECT* rect) const;
    RECT NonClientInsets() const;
    void SetClientSize(int width, int height);

    void UpdateCursor();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;

    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<VideoCapture> video_;
    std::unique_ptr<AudioPassthrough> audio_;
    std::unique_ptr<AudioDeviceWatcher> audioWatcher_;
    PictureDialog pictureDialog_;
    PictureSettings picture_;

    std::vector<VideoDeviceInfo> videoDevices_;
    std::vector<AudioInputInfo> audioInputs_;
    std::optional<VideoDeviceInfo> activeVideo_;
    std::optional<VideoMode> activeMode_;
    VideoCaps capabilities_;
    bool videoFailed_ = false;
    UINT videoGeneration_ = 0;
    std::wstring audioFailure_;
    bool cameraDenied_ = false;
    bool microphoneDenied_ = false;
    float volume_ = 1;
    bool muted_ = false;

    // Frame watchdog
    uint64_t lastFrameCount_ = 0;
    ULONGLONG lastFrameCheck_ = 0;
    double measuredFps_ = 0;
    int secondsWithoutFrames_ = 0;
    bool showingNoSignal_ = false;
    int watchdogTicks_ = 0;
    int audioRetryDelay_ = 5;  // watchdog ticks; doubles up to a minute while audio keeps failing
    int nextAudioRetry_ = 5;
    uint64_t lastInputGaps_ = 0;
    ULONGLONG lastInputGapTime_ = 0;
    bool sleepPrevented_ = false;

    // Window state
    bool fullScreen_ = false;
    bool topMost_ = false;
    WINDOWPLACEMENT savedPlacement_{sizeof(WINDOWPLACEMENT)};
    LONG_PTR savedStyle_ = 0;
    bool cursorHidden_ = false;
    POINT lastMouse_{-1, -1};
    ULONGLONG lastMouseMove_ = 0;
    bool menuOpen_ = false;

    // Context-menu entries built on the fly: command id -> value.
    std::vector<std::wstring> menuVideoLinks_;
    std::vector<std::optional<SIZE>> menuSizes_;
    std::vector<std::optional<double>> menuRates_;
    std::vector<std::wstring> menuAudioIds_;
};
