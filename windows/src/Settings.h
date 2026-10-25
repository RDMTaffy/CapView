// Persisted user choices (HKEY_CURRENT_USER\Software\CapView). std::nullopt = automatic.
#pragma once

#include "Common.h"

enum class AudioMode { Automatic, Off, Device };

struct AudioSelection {
    AudioMode mode = AudioMode::Automatic;
    std::wstring deviceId;  // WASAPI endpoint id when mode == Device
};

namespace Settings {

/// The capture device used last time: its symbolic link (changes with the USB port),
/// its USB "VID:PID" model and display name. All empty before the first successful start.
std::wstring VideoDeviceLink();
std::wstring VideoModel();
std::wstring VideoName();
void SetVideoDevice(const std::wstring& link, const std::wstring& model, const std::wstring& name);

std::optional<SIZE> Resolution();
void SetResolution(std::optional<SIZE> size);

std::optional<double> FrameRate();
void SetFrameRate(std::optional<double> fps);

AudioSelection Audio();
void SetAudio(const AudioSelection& selection);

float Volume();  // 0 ... 1, default 1
void SetVolume(float volume);

bool Muted();
void SetMuted(bool muted);

bool TopMost();
void SetTopMost(bool topMost);

/// Missing or out-of-range values fall back to defaults / are clamped to the slider ranges.
PictureSettings Picture();
void SetPicture(const PictureSettings& picture);

/// Last normal (non-fullscreen) window rectangle in screen pixels, if saved.
std::optional<RECT> WindowRect();
void SetWindowRect(const RECT& rect);

}  // namespace Settings
