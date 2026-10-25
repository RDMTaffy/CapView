// WASAPI passthrough: capture endpoint -> SPSC ring -> PI-steered resampler -> default render endpoint.
#pragma once

#include "Common.h"

struct AudioStatus {
    double latencyMs = 0;     // capture period + buffered audio + render padding + stream latencies
    uint64_t underruns = 0;   // output ran dry while input was flowing (raises the target)
    uint64_t inputGaps = 0;   // the capture device itself stopped delivering (discontinuity / glitch)
};

class AudioPassthrough {
public:
    /// Posts WM_APP_AUDIO_FAILED(lParam = new std::wstring reason) to `notify` when audio stops
    /// on its own (device invalidated, restart failed).
    explicit AudioPassthrough(HWND notify);
    ~AudioPassthrough();
    AudioPassthrough(const AudioPassthrough&) = delete;
    AudioPassthrough& operator=(const AudioPassthrough&) = delete;

    /// Starts playing capture endpoint `endpointId` on the current default render endpoint.
    /// Stops any previous stream first. UI thread. Waits at most ~10 s for the audio driver;
    /// returns HRESULT_FROM_WIN32(ERROR_TIMEOUT) when it hangs (and for a minute afterwards).
    HRESULT Start(const std::wstring& endpointId);
    void Stop();

    /// Endpoint id currently playing; empty when stopped (also after an internal failure).
    std::wstring InputId() const;

    void SetVolume(float volume);  // 0 ... 1; gain applied is volume^2
    void SetMuted(bool muted);

    /// nullopt when stopped.
    std::optional<AudioStatus> Status() const;

    /// UI thread, after WM_APP_DEFAULT_OUTPUT_CHANGED: rebuild only the render side.
    void OnDefaultOutputChanged();

private:
    /// After an audio thread got stuck in a driver call: hand the old state to it and start over.
    void RetireIfAbandoned();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    ULONGLONG retryAfter_ = 0;  // Start() refuses (ERROR_TIMEOUT) until then
};

/// Watches WASAPI endpoints (IMMNotificationClient) and posts WM_APP_AUDIO_DEVICES_CHANGED /
/// WM_APP_DEFAULT_OUTPUT_CHANGED (eRender, eConsole role) to `notify`.
class AudioDeviceWatcher {
public:
    explicit AudioDeviceWatcher(HWND notify);
    ~AudioDeviceWatcher();
    AudioDeviceWatcher(const AudioDeviceWatcher&) = delete;
    AudioDeviceWatcher& operator=(const AudioDeviceWatcher&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
