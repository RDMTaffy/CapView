// Media Foundation capture: newest frame straight to a sink, no queue.
#pragma once

#include "Common.h"

#include <map>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

struct VideoMode {
    UINT32 width = 0;
    UINT32 height = 0;
    double fps = 0;        // exact: numerator / denominator
    GUID subtype = GUID_NULL;  // delivered format: MFVideoFormat_NV12 or MFVideoFormat_YUY2
};

struct VideoCaps {
    std::vector<SIZE> sizes;  // distinct sizes, largest area first
    // Distinct frame rates per size (key: width << 32 | height), fastest first, rounded to 0.01.
    std::map<uint64_t, std::vector<double>> rates;
    std::vector<double> RatesFor(UINT32 w, UINT32 h) const {
        auto it = rates.find((uint64_t(w) << 32) | h);
        return it == rates.end() ? std::vector<double>{} : it->second;
    }
};

/// Called on a Media Foundation worker thread for every frame, in delivery order.
/// The sample is only valid during the call (AddRef it to keep it).
using FrameSink = std::function<void(IMFSample* sample, const VideoMode& mode)>;

class VideoCapture {
public:
    VideoCapture(HWND notify, FrameSink sink);
    ~VideoCapture();
    VideoCapture(const VideoCapture&) = delete;
    VideoCapture& operator=(const VideoCapture&) = delete;

    /// Opens `symbolicLink` and starts streaming at the requested size / rate
    /// (nullopt = automatic: prefer formats reaching min(requested or 50, best) fps, then the
    /// largest size, then the closest rate, then NV12 > YUY2 > MJPG(decoded to NV12)).
    /// Runs asynchronously; posts WM_APP_VIDEO_STARTED(wParam = returned generation, lParam = HRESULT).
    /// Stops any previous stream first.
    UINT Start(const std::wstring& symbolicLink, std::optional<SIZE> size, std::optional<double> fps);

    /// Stops streaming and releases the device (asynchronous, returns immediately).
    void Stop();

    /// Re-opens the current device with the current choice (used by the stall watchdog).
    void Restart();

    /// The mode the device is running at (valid after a successful start).
    VideoMode Mode() const;

    /// Frames delivered since construction.
    uint64_t FramesReceived() const;

    /// Formats of the device last opened by Start/Restart (recorded on the worker thread, so the
    /// UI never has to open the camera itself). Empty before the first successful open.
    VideoCaps Caps() const;

    /// Opens the device just to list its formats. Blocks; do not call while it is streaming.
    static VideoCaps Capabilities(const std::wstring& symbolicLink);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
