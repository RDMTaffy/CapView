// Persisted user choices in HKEY_CURRENT_USER\Software\CapView.
//
// Values are plain and regedit-friendly so a tester can inspect or reset them:
// strings and doubles as REG_SZ (doubles in locale-independent "C" notation, never with a
// decimal comma), booleans as REG_DWORD, the window rectangle as REG_BINARY.
// Missing or malformed values fall back to defaults; nothing here throws.
#include "Settings.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cwchar>

namespace {

constexpr wchar_t kKeyPath[] = L"Software\\CapView";

constexpr wchar_t kVideoDeviceLink[] = L"VideoDeviceLink";
constexpr wchar_t kVideoModel[] = L"VideoModel";
constexpr wchar_t kVideoName[] = L"VideoName";
constexpr wchar_t kResolution[] = L"Resolution";  // "1920x1080"; absent = automatic
constexpr wchar_t kFrameRate[] = L"FrameRate";    // "59.94"; absent = automatic
constexpr wchar_t kAudioMode[] = L"AudioMode";    // "auto" | "off" | "device"
constexpr wchar_t kAudioDevice[] = L"AudioDevice"; // endpoint id when AudioMode = "device"
constexpr wchar_t kVolume[] = L"Volume";
constexpr wchar_t kMuted[] = L"Muted";
constexpr wchar_t kTopMost[] = L"TopMost";
constexpr wchar_t kBrightness[] = L"PictureBrightness";
constexpr wchar_t kContrast[] = L"PictureContrast";
constexpr wchar_t kSaturation[] = L"PictureSaturation";
constexpr wchar_t kGamma[] = L"PictureGamma";
constexpr wchar_t kHue[] = L"PictureHue";
constexpr wchar_t kExpandRange[] = L"PictureExpandRange";
constexpr wchar_t kWindowRect[] = L"WindowRect";

constexpr DWORD kMaxStringBytes = 64 * 1024;  // anything larger is not ours

// Slider ranges, same as the macOS app (see PictureSettings in Common.h).
constexpr double kBrightnessMin = -0.3, kBrightnessMax = 0.3;
constexpr double kContrastMin = 0.5, kContrastMax = 1.5;
constexpr double kSaturationMin = 0.0, kSaturationMax = 2.0;
constexpr double kGammaMin = 0.5, kGammaMax = 1.5;
constexpr double kHueMin = -180.0, kHueMax = 180.0;

void LogStatus(LSTATUS status, const wchar_t* action, const wchar_t* name) {
    LogIfFailed(HRESULT_FROM_WIN32(static_cast<DWORD>(status)),
                (std::wstring(L"Settings: ") + action + L" " + name).c_str());
}

// A missing key or value is the normal "never saved" case and is not logged.
bool IsMissing(LSTATUS status) {
    return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

std::optional<std::wstring> ReadString(const wchar_t* name) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD bytes = 0;
        LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kKeyPath, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
        if (status != ERROR_SUCCESS) {
            if (!IsMissing(status)) LogStatus(status, L"read", name);
            return std::nullopt;
        }
        if (bytes > kMaxStringBytes) {
            Log(L"Settings: %ls is %lu bytes, ignoring it", name, static_cast<unsigned long>(bytes));
            return std::nullopt;
        }
        // One spare character: RegGetValueW null-terminates and may need room for it.
        std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
        DWORD capacity = static_cast<DWORD>(value.size() * sizeof(wchar_t));
        status = RegGetValueW(HKEY_CURRENT_USER, kKeyPath, name, RRF_RT_REG_SZ, nullptr, value.data(), &capacity);
        if (status == ERROR_MORE_DATA) continue;  // changed between the two calls
        if (status != ERROR_SUCCESS) {
            if (!IsMissing(status)) LogStatus(status, L"read", name);
            return std::nullopt;
        }
        value.resize(std::wcslen(value.c_str()));
        return value;
    }
    Log(L"Settings: %ls kept changing while being read, ignoring it", name);
    return std::nullopt;
}

std::optional<DWORD> ReadDword(const wchar_t* name) {
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kKeyPath, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    if (status != ERROR_SUCCESS) {
        if (!IsMissing(status)) LogStatus(status, L"read", name);
        return std::nullopt;
    }
    return value;
}

void WriteValue(const wchar_t* name, DWORD type, const void* data, DWORD bytes) {
    // RegSetKeyValueW creates Software\CapView on first use.
    LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, kKeyPath, name, type, data, bytes);
    if (status != ERROR_SUCCESS) LogStatus(status, L"write", name);
}

void WriteString(const wchar_t* name, const std::wstring& value) {
    const size_t bytes = (value.size() + 1) * sizeof(wchar_t);
    if (bytes > kMaxStringBytes) {
        Log(L"Settings: not saving %ls (%zu bytes)", name, bytes);
        return;
    }
    WriteValue(name, REG_SZ, value.c_str(), static_cast<DWORD>(bytes));
}

void WriteDword(const wchar_t* name, DWORD value) {
    WriteValue(name, REG_DWORD, &value, sizeof(value));
}

void DeleteValue(const wchar_t* name) {
    LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kKeyPath, name);
    if (status != ERROR_SUCCESS && !IsMissing(status)) LogStatus(status, L"delete", name);
}

// to_chars / from_chars ignore the user's locale, unlike printf / wcstod.
template <typename Number>
std::wstring FormatNumber(Number value) {
    char text[64];
    auto [end, error] = std::to_chars(text, text + sizeof(text), value);
    if (error != std::errc()) return L"0";
    return std::wstring(text, end);  // ASCII only, so widening char by char is exact
}

// The registry text as ASCII with surrounding blanks removed; empty if it contains anything
// that cannot be part of a number.
std::string AsciiTrimmed(const std::wstring& text) {
    size_t first = text.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    size_t last = text.find_last_not_of(L" \t");
    std::string ascii;
    for (size_t i = first; i <= last; ++i) {
        wchar_t c = text[i];
        if (c >= 0x80) return {};
        ascii += static_cast<char>(c == L',' ? L'.' : c);  // tolerate a hand-edited decimal comma
    }
    if (!ascii.empty() && ascii.front() == '+') ascii.erase(0, 1);  // from_chars rejects '+'
    return ascii;
}

std::optional<double> ParseDouble(const std::wstring& text) {
    const std::string ascii = AsciiTrimmed(text);
    if (ascii.empty()) return std::nullopt;
    double value = 0;
    const char* end = ascii.data() + ascii.size();
    auto [stop, error] = std::from_chars(ascii.data(), end, value);
    if (error != std::errc() || stop != end || !std::isfinite(value)) return std::nullopt;
    return value;
}

std::optional<long> ParseInteger(const std::string& ascii) {
    if (ascii.empty()) return std::nullopt;
    long value = 0;
    const char* end = ascii.data() + ascii.size();
    auto [stop, error] = std::from_chars(ascii.data(), end, value);
    if (error != std::errc() || stop != end) return std::nullopt;
    return value;
}

std::optional<double> ReadDouble(const wchar_t* name) {
    std::optional<std::wstring> text = ReadString(name);
    if (!text) return std::nullopt;
    std::optional<double> value = ParseDouble(*text);
    if (!value) Log(L"Settings: %ls = \"%ls\" is not a number, ignoring it", name, text->c_str());
    return value;
}

double ReadClamped(const wchar_t* name, double low, double high, double fallback) {
    std::optional<double> value = ReadDouble(name);
    return value ? std::clamp(*value, low, high) : fallback;
}

// Non-finite input would survive std::clamp, so it falls back to the default first.
double Clamped(double value, double low, double high, double fallback) {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

bool ReadBool(const wchar_t* name) {
    std::optional<DWORD> value = ReadDword(name);
    return value && *value != 0;
}

}  // namespace

namespace Settings {

std::wstring VideoDeviceLink() { return ReadString(kVideoDeviceLink).value_or(std::wstring()); }
std::wstring VideoModel() { return ReadString(kVideoModel).value_or(std::wstring()); }
std::wstring VideoName() { return ReadString(kVideoName).value_or(std::wstring()); }

void SetVideoDevice(const std::wstring& link, const std::wstring& model, const std::wstring& name) {
    WriteString(kVideoDeviceLink, link);
    WriteString(kVideoModel, model);
    WriteString(kVideoName, name);
}

std::optional<SIZE> Resolution() {
    std::optional<std::wstring> text = ReadString(kResolution);
    if (!text) return std::nullopt;
    const std::string ascii = AsciiTrimmed(*text);
    const size_t x = ascii.find_first_of("xX");
    if (x != std::string::npos) {
        std::optional<long> width = ParseInteger(ascii.substr(0, x));
        std::optional<long> height = ParseInteger(ascii.substr(x + 1));
        if (width && height && *width > 0 && *height > 0 && *width <= 16384 && *height <= 16384)
            return SIZE{*width, *height};
    }
    Log(L"Settings: Resolution = \"%ls\" is invalid, using automatic", text->c_str());
    return std::nullopt;
}

void SetResolution(std::optional<SIZE> size) {
    if (!size || size->cx <= 0 || size->cy <= 0) {
        DeleteValue(kResolution);
        return;
    }
    WriteString(kResolution, FormatNumber(size->cx) + L"x" + FormatNumber(size->cy));
}

std::optional<double> FrameRate() {
    std::optional<double> fps = ReadDouble(kFrameRate);
    if (fps && (*fps <= 0 || *fps > 1000)) {
        Log(L"Settings: FrameRate %.3f is out of range, using automatic", *fps);
        return std::nullopt;
    }
    return fps;
}

void SetFrameRate(std::optional<double> fps) {
    if (!fps || !std::isfinite(*fps) || *fps <= 0) {
        DeleteValue(kFrameRate);
        return;
    }
    WriteString(kFrameRate, FormatNumber(*fps));
}

AudioSelection Audio() {
    AudioSelection selection;
    const std::wstring mode = ReadString(kAudioMode).value_or(std::wstring(L"auto"));
    if (mode == L"off") {
        selection.mode = AudioMode::Off;
    } else if (mode == L"device") {
        selection.deviceId = ReadString(kAudioDevice).value_or(std::wstring());
        // A device choice without an id cannot be honoured; automatic is the safe reading.
        selection.mode = selection.deviceId.empty() ? AudioMode::Automatic : AudioMode::Device;
    }
    return selection;
}

void SetAudio(const AudioSelection& selection) {
    switch (selection.mode) {
    case AudioMode::Off:
        WriteString(kAudioMode, L"off");
        break;
    case AudioMode::Device:
        if (!selection.deviceId.empty()) {
            WriteString(kAudioDevice, selection.deviceId);
            WriteString(kAudioMode, L"device");
            break;
        }
        [[fallthrough]];
    case AudioMode::Automatic:
        WriteString(kAudioMode, L"auto");
        break;
    }
}

float Volume() {
    return static_cast<float>(ReadClamped(kVolume, 0.0, 1.0, 1.0));
}

void SetVolume(float volume) {
    // Formatted as float so 0.7f is stored as "0.7", not "0.699999988079071".
    const float value = std::isfinite(volume) ? std::clamp(volume, 0.0f, 1.0f) : 1.0f;
    WriteString(kVolume, FormatNumber(value));
}

bool Muted() { return ReadBool(kMuted); }
void SetMuted(bool muted) { WriteDword(kMuted, muted ? 1 : 0); }

bool TopMost() { return ReadBool(kTopMost); }
void SetTopMost(bool topMost) { WriteDword(kTopMost, topMost ? 1 : 0); }

PictureSettings Picture() {
    const PictureSettings neutral;
    PictureSettings picture;
    picture.brightness = ReadClamped(kBrightness, kBrightnessMin, kBrightnessMax, neutral.brightness);
    picture.contrast = ReadClamped(kContrast, kContrastMin, kContrastMax, neutral.contrast);
    picture.saturation = ReadClamped(kSaturation, kSaturationMin, kSaturationMax, neutral.saturation);
    picture.gamma = ReadClamped(kGamma, kGammaMin, kGammaMax, neutral.gamma);
    picture.hue = ReadClamped(kHue, kHueMin, kHueMax, neutral.hue);
    picture.expandRange = ReadBool(kExpandRange);
    return picture;
}

void SetPicture(const PictureSettings& picture) {
    // Called on every slider move: no logging on success.
    const PictureSettings neutral;
    WriteString(kBrightness, FormatNumber(Clamped(picture.brightness, kBrightnessMin, kBrightnessMax, neutral.brightness)));
    WriteString(kContrast, FormatNumber(Clamped(picture.contrast, kContrastMin, kContrastMax, neutral.contrast)));
    WriteString(kSaturation, FormatNumber(Clamped(picture.saturation, kSaturationMin, kSaturationMax, neutral.saturation)));
    WriteString(kGamma, FormatNumber(Clamped(picture.gamma, kGammaMin, kGammaMax, neutral.gamma)));
    WriteString(kHue, FormatNumber(Clamped(picture.hue, kHueMin, kHueMax, neutral.hue)));
    WriteDword(kExpandRange, picture.expandRange ? 1 : 0);
}

std::optional<RECT> WindowRect() {
    // Fixed little-endian layout {left, top, right, bottom} as 32-bit ints, independent of
    // how RECT happens to be declared.
    int32_t values[4] = {};
    DWORD bytes = sizeof(values);
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kKeyPath, kWindowRect, RRF_RT_REG_BINARY, nullptr, values, &bytes);
    if (status != ERROR_SUCCESS) {
        if (!IsMissing(status)) LogStatus(status, L"read", kWindowRect);
        return std::nullopt;
    }
    if (bytes != sizeof(values)) {
        Log(L"Settings: WindowRect has %lu bytes, ignoring it", static_cast<unsigned long>(bytes));
        return std::nullopt;
    }
    // Sanity only; whether it is still on a connected monitor is the window's decision.
    constexpr int32_t kLimit = 1 << 20;
    const RECT rect{values[0], values[1], values[2], values[3]};
    const bool sane = rect.right > rect.left && rect.bottom > rect.top && rect.left > -kLimit &&
                      rect.top > -kLimit && rect.right < kLimit && rect.bottom < kLimit;
    if (!sane) {
        Log(L"Settings: WindowRect (%ld, %ld, %ld, %ld) is invalid, ignoring it", rect.left, rect.top, rect.right,
            rect.bottom);
        return std::nullopt;
    }
    return rect;
}

void SetWindowRect(const RECT& rect) {
    const int32_t values[4] = {static_cast<int32_t>(rect.left), static_cast<int32_t>(rect.top),
                               static_cast<int32_t>(rect.right), static_cast<int32_t>(rect.bottom)};
    WriteValue(kWindowRect, REG_BINARY, values, sizeof(values));
}

}  // namespace Settings
