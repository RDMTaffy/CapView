// Shared includes, app messages and small helpers for CapView for Windows.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// Messages posted to the main window. Ownership rules for lParam are noted per message.
enum : UINT {
    // VideoCapture finished Start(): wParam = generation, lParam = HRESULT.
    WM_APP_VIDEO_STARTED = WM_APP + 1,
    // VideoCapture's reader reported an error while running: wParam = generation, lParam = HRESULT.
    WM_APP_VIDEO_ERROR,
    // Renderer finished a snapshot: lParam = SnapshotImage* (receiver deletes; nullptr if none).
    WM_APP_SNAPSHOT,
    // AudioPassthrough stopped on its own: lParam = std::wstring* reason (receiver deletes).
    WM_APP_AUDIO_FAILED,
    // Audio endpoints were added/removed/changed state.
    WM_APP_AUDIO_DEVICES_CHANGED,
    // The default render (output) endpoint changed.
    WM_APP_DEFAULT_OUTPUT_CHANGED,
    // PictureDialog changed a value (the dialog already called its callback; reserved).
    WM_APP_PICTURE_CHANGED,
    // VideoCapture finished a Restart() that was not superseded: wParam = generation, lParam = HRESULT.
    WM_APP_VIDEO_RESTARTED,
    // The delivered media type changed mid-stream: wParam = generation (read VideoCapture::Mode()).
    WM_APP_VIDEO_MODE_CHANGED,
};

/// Picture corrections, same meaning and ranges as the macOS app.
/// All math happens on gamma-encoded (sRGB-like) values in [0, 1].
struct PictureSettings {
    double brightness = 0.0;  // -0.3 ... 0.3, added after contrast
    double contrast = 1.0;    // 0.5 ... 1.5, pivot 0.5
    double saturation = 1.0;  // 0 ... 2, mix toward Rec.709 luma
    double gamma = 1.0;       // 0.5 ... 1.5, out = pow(in, 1 / gamma) (above 1 brightens)
    double hue = 0.0;         // -180 ... 180 degrees, rotation around the grey axis
    bool expandRange = false; // stretch 16-235 to 0-255 and clamp

    bool IsNeutral() const {
        return brightness == 0 && contrast == 1 && saturation == 1 && gamma == 1 && hue == 0 && !expandRange;
    }
    bool operator==(const PictureSettings&) const = default;
};

/// A captured frame for the clipboard: top-down BGRA8, width * 4 bytes per row.
struct SnapshotImage {
    UINT width = 0;
    UINT height = 0;
    std::vector<uint8_t> bgra;
};

/// Appends a line to %LOCALAPPDATA%\CapView\capview.log (thread-safe, timestamped).
/// Used heavily so a tester can send the log back.
void Log(const wchar_t* format, ...);

/// Logs `what` with the HRESULT if it failed; returns hr unchanged.
HRESULT LogIfFailed(HRESULT hr, const wchar_t* what);

/// Folder holding capview.log (created on first use).
std::wstring LogFolder();

/// A worker stuck in a driver call was left running instead of being waited for. wWinMain then
/// ends the process with TerminateProcess: a normal exit could block on that driver again.
void NoteAbandonedThread();
bool AnyThreadAbandoned();

std::wstring Utf8ToWide(const std::string& text);
std::string WideToUtf8(const std::wstring& text);

/// "{vid}:{pid}" in upper-case hex (e.g. "07CA:3112") parsed from a Windows device path or
/// instance id containing "VID_07CA&PID_3112" (case-insensitive); empty if not found.
std::wstring UsbIdsFromPath(const std::wstring& path);
