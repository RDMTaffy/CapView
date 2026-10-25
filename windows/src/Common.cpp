// Logging, UTF-8 conversion and USB id parsing shared by every module.
#include "Common.h"

#include <audioclient.h>

#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace {

constexpr ULONGLONG kMaxLogBytes = 1024 * 1024;  // rotate at first open past this size
constexpr size_t kMaxLineChars = 64 * 1024;      // longer messages are cut, never dropped

// All of this is constant-initialised (zero / SRWLOCK_INIT), so Log works from any static
// constructor and from threads still running during shutdown. The handle is never closed:
// the OS closes it at exit, which avoids a use-after-close race with late loggers.
SRWLOCK g_logLock = SRWLOCK_INIT;
HANDLE g_logFile = nullptr;  // guarded by g_logLock; nullptr = not open
bool g_logOpenTried = false; // guarded by g_logLock; open only once even if it failed

std::wstring EnvironmentValue(const wchar_t* name) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    DWORD got = GetEnvironmentVariableW(name, value.data(), needed);
    if (got == 0 || got >= needed) return {};
    value.resize(got);
    return value;
}

bool EnsureDirectory(const std::wstring& dir) {
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    DWORD attributes = GetFileAttributesW(dir.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring WithTrailingSlash(std::wstring path) {
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    return path;
}

// %LOCALAPPDATA%\CapView, or %TEMP%\CapView if that cannot be created (roaming/locked
// profiles). Uses the environment rather than SHGetKnownFolderPath to stay free of shell32.
std::wstring ComputeLogFolder() {
    std::wstring base = EnvironmentValue(L"LOCALAPPDATA");
    if (!base.empty()) {
        std::wstring dir = WithTrailingSlash(base) + L"CapView";
        if (EnsureDirectory(dir)) return dir;
    }
    wchar_t temp[MAX_PATH + 2] = {};
    DWORD length = GetTempPathW(MAX_PATH + 2, temp);
    if (length > 0 && length < MAX_PATH + 2) {
        std::wstring tempDir(temp, length);
        std::wstring dir = WithTrailingSlash(tempDir) + L"CapView";
        if (EnsureDirectory(dir)) return dir;
        while (!tempDir.empty() && (tempDir.back() == L'\\' || tempDir.back() == L'/')) tempDir.pop_back();
        if (!tempDir.empty()) return tempDir;
    }
    return L".";
}

void WriteUtf8Locked(const std::wstring& line) {
    if (!g_logFile) return;
    std::string bytes = WideToUtf8(line);
    DWORD written = 0;
    WriteFile(g_logFile, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

std::wstring Timestamped(const std::wstring& message) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t prefix[64];
    std::swprintf(prefix, 64, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%5lu] ", unsigned(t.wYear),
                  unsigned(t.wMonth), unsigned(t.wDay), unsigned(t.wHour), unsigned(t.wMinute),
                  unsigned(t.wSecond), unsigned(t.wMilliseconds), static_cast<unsigned long>(GetCurrentThreadId()));
    return prefix + message + L"\r\n";
}

// The real Windows build number (GetVersionEx lies to unmanifested processes).
std::wstring WindowsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    FARPROC proc = ntdll ? GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
    if (!proc) return L"unknown";
    // Through void(*)() so GCC's -Wcast-function-type stays quiet; MSVC accepts it too.
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void (*)()>(proc));
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0) return L"unknown";
    wchar_t text[64];
    std::swprintf(text, 64, L"%lu.%lu.%lu", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
    return text;
}

void OpenLogLocked() {
    g_logOpenTried = true;
    const std::wstring folder = ComputeLogFolder();
    const std::wstring path = folder + L"\\capview.log";

    // Rotate once per process: keep the previous log as capview.old.log so a tester who
    // relaunches after a problem still has it. If renaming fails, truncate instead.
    bool rotated = false;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        ULONGLONG size = (ULONGLONG(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (size > kMaxLogBytes) {
            const std::wstring old = folder + L"\\capview.old.log";
            rotated = MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
            if (!rotated) {
                // Truncation needs write-data access, which the append-only handle below lacks.
                HANDLE truncate = CreateFileW(path.c_str(), GENERIC_WRITE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                              TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (truncate != INVALID_HANDLE_VALUE) CloseHandle(truncate);
            }
        }
    }

    // FILE_APPEND_DATA only: every WriteFile lands at the end, even with a second instance
    // writing the same file. Sharing lets Notepad read it while CapView runs.
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        OutputDebugStringW((L"CapView: cannot open log file " + path + L"\n").c_str());
        return;
    }
    g_logFile = file;

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t header[256];
    std::swprintf(header, 256, L"==== CapView log opened: pid %lu, Windows %ls%ls",
                  static_cast<unsigned long>(GetCurrentProcessId()), WindowsVersion().c_str(),
                  rotated ? L", previous log moved to capview.old.log" : L"");
    WriteUtf8Locked(Timestamped(header));
    WriteUtf8Locked(Timestamped(std::wstring(L"executable: ") + exe));
}

bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

// Log's format strings follow the MSVC wide-printf convention (%s / %c are wide, %S / %hs
// are narrow). mingw's ISO printf reads a bare %s as char*, so every string or char
// conversion is rewritten with an explicit h (narrow) or l (wide), which both CRTs agree on.
std::wstring NormalizeFormat(const wchar_t* f) {
    std::wstring out;
    out.reserve(std::wcslen(f) + 16);
    while (*f) {
        if (*f != L'%') {
            out += *f++;
            continue;
        }
        out += *f++;
        if (*f == L'%') {
            out += *f++;
            continue;
        }
        while (*f && std::wcschr(L"-+ #0'", *f)) out += *f++;
        while (*f == L'*' || IsDigit(*f)) out += *f++;
        if (*f == L'.') {
            out += *f++;
            while (*f == L'*' || IsDigit(*f)) out += *f++;
        }
        std::wstring length;
        while (*f && std::wcschr(L"hlLqjztwI", *f)) {
            if (*f == L'I' && ((f[1] == L'3' && f[2] == L'2') || (f[1] == L'6' && f[2] == L'4'))) {
                length.append(f, 3);
                f += 3;
            } else {
                length += *f++;
            }
        }
        const wchar_t conversion = *f;
        if (!conversion) {
            out += length;
            break;
        }
        ++f;
        const bool isString = conversion == L's' || conversion == L'S';
        const bool isChar = conversion == L'c' || conversion == L'C';
        if (isString || isChar) {
            bool narrow;
            if (length == L"h" || length == L"hh") narrow = true;
            else if (length == L"l" || length == L"w" || length == L"ll") narrow = false;
            else narrow = (conversion == L'S' || conversion == L'C');  // MSVC: upper case = other width
            out += narrow ? L'h' : L'l';
            out += isString ? L's' : L'c';
        } else {
            out += length;
            out += conversion;
        }
    }
    return out;
}

std::wstring FormatMessageV(const wchar_t* format, va_list args) {
    if (!format) return L"(null format)";
    const std::wstring normalized = NormalizeFormat(format);
    std::wstring buffer(512, L'\0');
    for (;;) {
        va_list copy;
        va_copy(copy, args);
        int n = std::vswprintf(buffer.data(), buffer.size(), normalized.c_str(), copy);
        va_end(copy);
        if (n >= 0 && static_cast<size_t>(n) < buffer.size()) {
            buffer.resize(static_cast<size_t>(n));
            return buffer;
        }
        // Negative means "did not fit" (or a bad format); grow until the cap, then give up
        // and log the format itself so the line is not lost.
        if (buffer.size() >= kMaxLineChars) return L"[unformattable log line] " + std::wstring(format);
        buffer.assign(buffer.size() * 2, L'\0');
    }
}

void TrimTrailing(std::wstring& text) {
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'\t'))
        text.pop_back();
}

std::wstring MessageFromTable(DWORD code, HMODULE module) {
    wchar_t* text = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS |
                  (module ? FORMAT_MESSAGE_FROM_HMODULE : FORMAT_MESSAGE_FROM_SYSTEM);
    DWORD length = FormatMessageW(flags, module, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring result;
    if (length && text) result.assign(text, length);
    if (text) LocalFree(text);
    TrimTrailing(result);
    return result;
}

// WASAPI errors have no system message text; their names are what people search for.
const wchar_t* AudioClientErrorName(HRESULT hr) {
    switch (hr) {
    case AUDCLNT_E_NOT_INITIALIZED: return L"AUDCLNT_E_NOT_INITIALIZED";
    case AUDCLNT_E_ALREADY_INITIALIZED: return L"AUDCLNT_E_ALREADY_INITIALIZED";
    case AUDCLNT_E_WRONG_ENDPOINT_TYPE: return L"AUDCLNT_E_WRONG_ENDPOINT_TYPE";
    case AUDCLNT_E_DEVICE_INVALIDATED: return L"AUDCLNT_E_DEVICE_INVALIDATED";
    case AUDCLNT_E_NOT_STOPPED: return L"AUDCLNT_E_NOT_STOPPED";
    case AUDCLNT_E_BUFFER_TOO_LARGE: return L"AUDCLNT_E_BUFFER_TOO_LARGE";
    case AUDCLNT_E_OUT_OF_ORDER: return L"AUDCLNT_E_OUT_OF_ORDER";
    case AUDCLNT_E_UNSUPPORTED_FORMAT: return L"AUDCLNT_E_UNSUPPORTED_FORMAT";
    case AUDCLNT_E_INVALID_SIZE: return L"AUDCLNT_E_INVALID_SIZE";
    case AUDCLNT_E_DEVICE_IN_USE: return L"AUDCLNT_E_DEVICE_IN_USE";
    case AUDCLNT_E_BUFFER_OPERATION_PENDING: return L"AUDCLNT_E_BUFFER_OPERATION_PENDING";
    case AUDCLNT_E_THREAD_NOT_REGISTERED: return L"AUDCLNT_E_THREAD_NOT_REGISTERED";
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return L"AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED";
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED: return L"AUDCLNT_E_ENDPOINT_CREATE_FAILED";
    case AUDCLNT_E_SERVICE_NOT_RUNNING: return L"AUDCLNT_E_SERVICE_NOT_RUNNING";
    case AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED: return L"AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED";
    case AUDCLNT_E_EXCLUSIVE_MODE_ONLY: return L"AUDCLNT_E_EXCLUSIVE_MODE_ONLY";
    case AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL: return L"AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL";
    case AUDCLNT_E_EVENTHANDLE_NOT_SET: return L"AUDCLNT_E_EVENTHANDLE_NOT_SET";
    case AUDCLNT_E_INCORRECT_BUFFER_SIZE: return L"AUDCLNT_E_INCORRECT_BUFFER_SIZE";
    case AUDCLNT_E_BUFFER_SIZE_ERROR: return L"AUDCLNT_E_BUFFER_SIZE_ERROR";
    case AUDCLNT_E_CPUUSAGE_EXCEEDED: return L"AUDCLNT_E_CPUUSAGE_EXCEEDED";
    case AUDCLNT_E_BUFFER_ERROR: return L"AUDCLNT_E_BUFFER_ERROR";
    case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED: return L"AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED";
    case AUDCLNT_E_INVALID_DEVICE_PERIOD: return L"AUDCLNT_E_INVALID_DEVICE_PERIOD";
    case AUDCLNT_E_RESOURCES_INVALIDATED: return L"AUDCLNT_E_RESOURCES_INVALIDATED";
    default: return nullptr;
    }
}

std::wstring DescribeHResult(HRESULT hr) {
    if (const wchar_t* name = AudioClientErrorName(hr)) return name;
    const DWORD code = static_cast<DWORD>(hr);
    std::wstring text = MessageFromTable(code, nullptr);
    if (text.empty() && HRESULT_FACILITY(hr) == FACILITY_WIN32) text = MessageFromTable(HRESULT_CODE(hr), nullptr);
    if (text.empty() && HRESULT_FACILITY(hr) == 0xD /* FACILITY_MF */) {
        // Media Foundation keeps its messages in mferror.dll. Loaded once, never freed.
        static const HMODULE mferror =
            LoadLibraryExW(L"mferror.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (mferror) text = MessageFromTable(code, mferror);
    }
    return text.empty() ? std::wstring(L"no description") : text;
}

bool IsHexDigit(wchar_t c) {
    return IsDigit(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

wchar_t AsciiUpper(wchar_t c) { return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c; }

bool MatchesAt(const std::wstring& text, size_t at, const wchar_t* literal) {
    for (size_t i = 0; literal[i]; ++i) {
        if (at + i >= text.size() || AsciiUpper(text[at + i]) != literal[i]) return false;
    }
    return true;
}

// Exactly four hex digits at `at` (a fifth digit means it is some other number).
bool FourHexAt(const std::wstring& text, size_t at) {
    if (at + 4 > text.size()) return false;
    for (size_t i = 0; i < 4; ++i) {
        if (!IsHexDigit(text[at + i])) return false;
    }
    return at + 4 == text.size() || !IsHexDigit(text[at + 4]);
}

std::wstring UpperHex4(const std::wstring& text, size_t at) {
    std::wstring result = text.substr(at, 4);
    for (wchar_t& c : result) c = AsciiUpper(c);
    return result;
}

}  // namespace

void Log(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    std::wstring message = FormatMessageV(format, args);
    va_end(args);
    TrimTrailing(message);  // callers sometimes end with "\n"; every line gets exactly one CRLF
    const std::wstring line = Timestamped(message);

    OutputDebugStringW(line.c_str());
    AcquireSRWLockExclusive(&g_logLock);
    if (!g_logOpenTried) OpenLogLocked();
    WriteUtf8Locked(line);
    ReleaseSRWLockExclusive(&g_logLock);
}

HRESULT LogIfFailed(HRESULT hr, const wchar_t* what) {
    if (SUCCEEDED(hr)) return hr;
    Log(L"FAILED %ls: hr=0x%08lX (%ls)", what ? what : L"(unnamed call)", static_cast<unsigned long>(hr),
        DescribeHResult(hr).c_str());
    return hr;
}

std::wstring LogFolder() {
    return ComputeLogFolder();
}

namespace {
std::atomic<bool> g_threadAbandoned{false};
}

void NoteAbandonedThread() {
    g_threadAbandoned = true;
}

bool AnyThreadAbandoned() {
    return g_threadAbandoned;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty() || text.size() > static_cast<size_t>(INT_MAX)) return {};
    const int bytes = static_cast<int>(text.size());
    // No MB_ERR_INVALID_CHARS: bad sequences become U+FFFD instead of losing the whole string.
    int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), bytes, nullptr, 0);
    if (chars <= 0) return {};
    std::wstring result(static_cast<size_t>(chars), L'\0');
    chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), bytes, result.data(), chars);
    if (chars <= 0) return {};
    result.resize(static_cast<size_t>(chars));
    return result;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty() || text.size() > static_cast<size_t>(INT_MAX)) return {};
    const int chars = static_cast<int>(text.size());
    // CP_UTF8 requires null default-char arguments; lone surrogates become U+FFFD.
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), chars, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), chars, result.data(), bytes, nullptr, nullptr);
    if (bytes <= 0) return {};
    result.resize(static_cast<size_t>(bytes));
    return result;
}

std::wstring UsbIdsFromPath(const std::wstring& path) {
    // e.g. \\?\usb#vid_07ca&pid_3112&mi_00#7&2a1b...#{e5323777-...}\global
    //      USB\VID_07CA&PID_3112&MI_02\8&1F4...
    for (size_t at = path.find_first_of(L"Vv"); at != std::wstring::npos; at = path.find_first_of(L"Vv", at + 1)) {
        if (!MatchesAt(path, at, L"VID_")) continue;
        const size_t vid = at + 4;
        if (!FourHexAt(path, vid)) continue;
        size_t pidTag = vid + 4;
        if (pidTag < path.size() && (path[pidTag] == L'&' || path[pidTag] == L'_')) ++pidTag;
        if (!MatchesAt(path, pidTag, L"PID_") || !FourHexAt(path, pidTag + 4)) continue;
        return UpperHex4(path, vid) + L":" + UpperHex4(path, pidTag + 4);
    }
    return {};
}
