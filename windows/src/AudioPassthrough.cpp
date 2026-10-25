// WASAPI audio passthrough, a port of the macOS PassthroughCore (see AudioPassthrough.h).
//
//   capture endpoint ─► capture thread ─► SPSC ring ─► render thread (PI-steered resampler) ─► default output
//
// Threads:
//   - UI thread (STA): Start/Stop/OnDefaultOutputChanged/Status. Owns the two audio threads.
//   - capture thread: event-driven shared IAudioCaptureClient in the endpoint's mix format,
//     converted to float stereo and written to the ring.
//   - render thread: event-driven shared IAudioRenderClient (float stereo at the capture rate; the
//     engine converts to the device format), fed by a linear-interpolation resampler whose step a
//     PI controller keeps within 1 ± 1000 ppm so the ring fill stays at a small target. That absorbs
//     the clock drift between the capture card and the output device.
//
// Only the capture thread moves `wpos`, only the render thread moves `rpos`. Both audio threads
// run MMCSS "Pro Audio", create and use their own COM objects (MTA) and never allocate, lock or log
// inside their loops; they log only while opening, rebuilding or failing.

#include "AudioPassthrough.h"

#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <limits>
#include <new>

namespace {

// Controller constants, identical to PassthroughCore.c (frames are capture-rate frames).
constexpr double kKpPpmPerFrame = 4.0;        // proportional gain
constexpr double kKiPpmPerFrameS = 0.2;       // integral gain (ζ ≈ 1, ωn ≈ 0.1 rad/s at 48 kHz)
constexpr double kMaxPpm = 1000.0;
constexpr double kFillEmaSeconds = 0.5;
constexpr double kResyncSlackFrames = 2400;   // this far above target => drop the backlog
constexpr uint32_t kMarginFrames = 128;       // slack on top of one period per side
constexpr uint32_t kTargetStepFrames = 64;    // added after each jitter underrun
constexpr uint32_t kTargetMaxExtra = 2048;
constexpr double kTargetDecaySeconds = 60.0;  // quiet time before giving some of it back
constexpr uint32_t kTargetDecayFrames = 16;
constexpr double kDecayHeadroomFrames = 48;   // spare frames that must remain after a decay step

constexpr double kRingSeconds = 0.34;
constexpr uint32_t kMinRingFrames = 8192;
constexpr uint32_t kMaxRingFrames = 1u << 22;
constexpr double kHnsPerSecond = 1e7;
constexpr REFERENCE_TIME kRenderBufferHns = 100000;   // 10 ms: the render buffer is the output latency
// 100 ms: room for a late wake-up, costs no latency (packets still arrive every period and are
// drained at once). Too small, and a late wake-up loses packets, which looks like a device gap.
constexpr REFERENCE_TIME kCaptureBufferHns = 1000000;
constexpr REFERENCE_TIME kFallbackPeriodHns = 100000;
constexpr DWORD kPollMs = 500;       // wake up without events too, so a dead device is still noticed
constexpr DWORD kSlowWaitMs = 3000;      // log (then keep waiting) when a driver call takes this long
constexpr DWORD kGiveUpWaitMs = 10000;   // then leave the audio thread behind: the UI must not hang on a driver
constexpr DWORD kStuckWaitMs = 1000;     // stopping after a thread already missed its handshake
constexpr ULONGLONG kAbandonedRetryPauseMs = 60000;
constexpr DWORD kRenderRetryDelaysMs[] = {0, 100, 250, 500, 1000, 2000};
constexpr int kMaxQuickRenderFailures = 5;
constexpr uint64_t kHealthyRenderCallbacks = 50;

static_assert(std::atomic<double>::is_always_lock_free, "audio threads must not take hidden locks");
static_assert(std::atomic<int64_t>::is_always_lock_free, "audio threads must not take hidden locks");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "audio threads must not take hidden locks");

// PKEY_Device_FriendlyName, spelled out so no propkey symbols need linking.
const PROPERTYKEY kFriendlyNameKey = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

class UniqueHandle {
public:
    UniqueHandle() = default;
    ~UniqueHandle() { Reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE Get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }
    void Reset(HANDLE handle = nullptr) {
        if (handle_) CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

struct CoTaskMemDeleter {
    void operator()(void* p) const { CoTaskMemFree(p); }
};
template <class T>
using UniqueCoMem = std::unique_ptr<T, CoTaskMemDeleter>;

std::wstring Hex(HRESULT hr) {
    wchar_t text[16];
    std::swprintf(text, 16, L"0x%08lX", static_cast<unsigned long>(hr));
    return text;
}

double HnsToMs(REFERENCE_TIME hns) {
    return static_cast<double>(hns) / 1e4;
}

std::wstring EndpointId(IMMDevice* device) {
    LPWSTR id = nullptr;
    std::wstring result;
    if (SUCCEEDED(device->GetId(&id)) && id) result = id;
    CoTaskMemFree(id);
    return result;
}

std::wstring EndpointName(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(store->GetValue(kFriendlyNameKey, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

// KSDATAFORMAT_SUBTYPE_xxx is {tag-0000-0010-8000-00AA00389B71}. Built and compared by hand so the
// ksmedia GUID symbols (and their link requirements) are not needed.
constexpr unsigned char kWaveSubtypeTail[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

bool IsWaveSubtype(const GUID& guid, WORD tag) {
    return guid.Data1 == tag && guid.Data2 == 0x0000 && guid.Data3 == 0x0010 &&
           std::memcmp(guid.Data4, kWaveSubtypeTail, sizeof(kWaveSubtypeTail)) == 0;
}

GUID WaveSubtype(WORD tag) {
    GUID guid{tag, 0x0000, 0x0010, {}};
    std::memcpy(guid.Data4, kWaveSubtypeTail, sizeof(kWaveSubtypeTail));
    return guid;
}

const WAVEFORMATEXTENSIBLE* AsExtensible(const WAVEFORMATEX& format) {
    if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE) return nullptr;
    if (size_t(format.cbSize) < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return nullptr;
    return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&format);
}

bool IsFloatFormat(const WAVEFORMATEX& format) {
    if (const auto* ext = AsExtensible(format)) return IsWaveSubtype(ext->SubFormat, WAVE_FORMAT_IEEE_FLOAT);
    return format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
}

bool IsPcmFormat(const WAVEFORMATEX& format) {
    if (const auto* ext = AsExtensible(format)) return IsWaveSubtype(ext->SubFormat, WAVE_FORMAT_PCM);
    return format.wFormatTag == WAVE_FORMAT_PCM;
}

std::wstring DescribeFormat(const WAVEFORMATEX& format) {
    const auto* ext = AsExtensible(format);
    const wchar_t* kind = IsFloatFormat(format) ? L"float" : IsPcmFormat(format) ? L"PCM" : L"other";
    wchar_t text[192];
    std::swprintf(text, 192, L"%lu Hz, %u ch, %u-bit %ls (valid %u, block %u, tag 0x%04X, mask 0x%lX)",
                  static_cast<unsigned long>(format.nSamplesPerSec), unsigned(format.nChannels),
                  unsigned(format.wBitsPerSample), kind,
                  unsigned(ext ? ext->Samples.wValidBitsPerSample : format.wBitsPerSample),
                  unsigned(format.nBlockAlign), unsigned(format.wFormatTag),
                  static_cast<unsigned long>(ext ? ext->dwChannelMask : 0));
    return text;
}

// MARK: - Capture format conversion

enum class SampleType { Float32, Float64, Int16, Int24, Int32 };

struct CaptureFormat {
    SampleType type = SampleType::Float32;
    UINT32 channels = 0;
    UINT32 blockAlign = 0;
    UINT32 bytesPerSample = 0;
    UINT32 rate = 0;
};

std::optional<CaptureFormat> ParseCaptureFormat(const WAVEFORMATEX& format) {
    CaptureFormat f;
    f.channels = format.nChannels;
    f.blockAlign = format.nBlockAlign;
    f.rate = format.nSamplesPerSec;
    f.bytesPerSample = format.wBitsPerSample / 8u;
    const bool isFloat = IsFloatFormat(format);
    const bool isPcm = IsPcmFormat(format);
    if (isFloat && format.wBitsPerSample == 32) {
        f.type = SampleType::Float32;
    } else if (isFloat && format.wBitsPerSample == 64) {
        f.type = SampleType::Float64;
    } else if (isPcm && format.wBitsPerSample == 16) {
        f.type = SampleType::Int16;
    } else if (isPcm && format.wBitsPerSample == 24) {
        f.type = SampleType::Int24;
    } else if (isPcm && format.wBitsPerSample == 32) {
        // 24 valid bits in a 32-bit container are MSB-aligned, so full-scale int32 math fits both.
        f.type = SampleType::Int32;
    } else {
        return std::nullopt;
    }
    if (f.channels == 0 || f.rate == 0 || f.blockAlign < f.channels * f.bytesPerSample) return std::nullopt;
    return f;
}

template <SampleType T>
float ReadSample(const BYTE* p) {
    if constexpr (T == SampleType::Float32) {
        float v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    } else if constexpr (T == SampleType::Float64) {
        double v;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(v);
    } else if constexpr (T == SampleType::Int16) {
        int16_t v;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(v) * (1.0f / 32768.0f);
    } else if constexpr (T == SampleType::Int24) {
        // Little-endian, packed: place it in the top 24 bits and scale as int32.
        const auto v = static_cast<int32_t>(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24);
        return static_cast<float>(v) * (1.0f / 2147483648.0f);
    } else {
        int32_t v;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(v) * (1.0f / 2147483648.0f);
    }
}

template <SampleType T>
void ConvertFrames(const BYTE* src, UINT32 frames, UINT32 blockAlign, UINT32 rightOffset, float* dst) {
    for (UINT32 i = 0; i < frames; i++) {
        const BYTE* frame = src + size_t(i) * blockAlign;
        dst[2 * size_t(i)] = ReadSample<T>(frame);
        dst[2 * size_t(i) + 1] = ReadSample<T>(frame + rightOffset);
    }
}

/// `frames` frames of `f` to interleaved float stereo. Mono is duplicated; channels after the
/// first two (front left/right in WAVE order) are dropped.
void ConvertToStereo(const BYTE* src, UINT32 frames, const CaptureFormat& f, float* dst) {
    const UINT32 right = f.channels > 1 ? f.bytesPerSample : 0;
    switch (f.type) {
    case SampleType::Float32: ConvertFrames<SampleType::Float32>(src, frames, f.blockAlign, right, dst); break;
    case SampleType::Float64: ConvertFrames<SampleType::Float64>(src, frames, f.blockAlign, right, dst); break;
    case SampleType::Int16: ConvertFrames<SampleType::Int16>(src, frames, f.blockAlign, right, dst); break;
    case SampleType::Int24: ConvertFrames<SampleType::Int24>(src, frames, f.blockAlign, right, dst); break;
    case SampleType::Int32: ConvertFrames<SampleType::Int32>(src, frames, f.blockAlign, right, dst); break;
    }
}

// MARK: - Thread setup and failure texts

/// COM (MTA) and MMCSS "Pro Audio" for the lifetime of an audio thread. Declared first in the
/// thread function so every COM object is released before CoUninitialize.
class AudioThreadScope {
public:
    explicit AudioThreadScope(const wchar_t* name) {
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr_)) Log(L"%ls thread: CoInitializeEx(MTA) failed (%ls)", name, Hex(hr_).c_str());
        DWORD taskIndex = 0;
        mmcss_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (mmcss_) {
            Log(L"%ls thread: MMCSS \"Pro Audio\" (task %lu)", name, static_cast<unsigned long>(taskIndex));
        } else {
            // Without MMCSS (service disabled), time-critical priority is the next best thing.
            Log(L"%ls thread: AvSetMmThreadCharacteristicsW failed (error %lu); using time-critical priority",
                name, static_cast<unsigned long>(GetLastError()));
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        }
    }
    ~AudioThreadScope() {
        if (mmcss_) AvRevertMmThreadCharacteristics(mmcss_);
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    AudioThreadScope(const AudioThreadScope&) = delete;
    AudioThreadScope& operator=(const AudioThreadScope&) = delete;

    HRESULT Result() const { return hr_; }

private:
    HRESULT hr_ = E_FAIL;
    HANDLE mmcss_ = nullptr;
};

std::wstring WithCode(const wchar_t* text, HRESULT hr) {
    return std::wstring(text) + L" (" + Hex(hr) + L")";
}

std::wstring CaptureFailureReason(HRESULT hr) {
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return L"오디오 입력 장치가 분리되었거나 설정이 바뀌었습니다.";
    if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) return L"Windows 오디오 서비스가 실행 중이 아닙니다.";
    return WithCode(L"오디오 입력이 중단되었습니다", hr);
}

std::wstring RenderFailureReason(HRESULT hr) {
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return L"소리를 낼 출력 장치가 없습니다.";
    if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) return L"Windows 오디오 서비스가 실행 중이 아닙니다.";
    return WithCode(L"오디오 출력을 다시 시작하지 못했습니다", hr);
}

/// Opts the stream's session out of Windows "ducking": a capture stream on what is often also the
/// default communications device must not turn other apps down, and our output must not be
/// turned down when a voice chat starts. Best effort.
void OptOutOfDucking(IAudioClient* client, const wchar_t* side) {
    ComPtr<IAudioSessionControl> control;
    ComPtr<IAudioSessionControl2> control2;
    HRESULT hr = client->GetService(IID_PPV_ARGS(&control));
    if (SUCCEEDED(hr)) hr = control.As(&control2);
    if (SUCCEEDED(hr)) hr = control2->SetDuckingPreference(TRUE);
    if (SUCCEEDED(hr)) {
        Log(L"%ls: opted out of ducking", side);
    } else {
        Log(L"%ls: ducking opt-out failed (%ls)", side, Hex(hr).c_str());
    }
}

}  // namespace

// MARK: - AudioPassthrough::Impl

struct AudioPassthrough::Impl {
    explicit Impl(HWND notifyWindow);
    ~Impl() { Stop(); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // UI thread.
    HRESULT Start(const std::wstring& id);
    void Stop();
    void OnDefaultOutputChanged();
    std::optional<AudioStatus> Status();
    void ApplyGain() {
        // Squared so the volume steps feel even to the ear.
        gain.store(muted ? 0.0f : volume * volume, std::memory_order_relaxed);
    }

    // Capture thread.
    struct CaptureStream {
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        UniqueHandle event;
        CaptureFormat format;
        UINT32 bufferFrames = 0;
        std::vector<float> scratch;  // interleaved stereo, bufferFrames frames
    };
    static DWORD WINAPI CaptureProc(void* self);
    void CaptureMain();
    HRESULT OpenCapture(CaptureStream& s);
    HRESULT CaptureLoop(CaptureStream& s);
    void RingWrite(const float* src, UINT32 n);

    // Render thread.
    struct RenderStream {
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        UniqueHandle event;
        std::wstring endpointId;
        UINT32 bufferFrames = 0;
        uint64_t callbacks = 0;
        bool started = false;

        ~RenderStream() { Close(); }
        void Close() {
            if (client && started) client->Stop();
            render.Reset();
            client.Reset();
            event.Reset();
            endpointId.clear();
            bufferFrames = 0;
            callbacks = 0;
            started = false;
        }
    };
    struct Controller {
        double rate = 48000;
        double renderChunk = 480;  // frames the render side takes per event (one engine period)
        double framesPerQpcTick = 0;
        double avgFill = 0;
        double integ = 0;
        double quietSeconds = 0;
        double minHeadroom = 0;    // smallest spare fill seen since the last decay check
        double frac = 0;           // resampler phase between rpos and rpos + 1
        uint32_t extraTarget = 0;
        bool running = false;
        bool underrunPending = false;  // ran dry; classified once audio flows again
        bool placeReadPosition = false;  // first event after (re)opening: choose where to start reading
        uint64_t gapsAtUnderrun = 0;
    };
    static DWORD WINAPI RenderProc(void* self);
    void RenderMain();
    HRESULT OpenRender(IMMDeviceEnumerator* enumerator, RenderStream& s, Controller& c);
    HRESULT RebuildRender(IMMDeviceEnumerator* enumerator, RenderStream& s, Controller& c, bool lost);
    HRESULT RenderLoop(IMMDeviceEnumerator* enumerator, RenderStream& s, Controller& c);
    HRESULT RenderOnce(RenderStream& s, Controller& c);
    void ResetController(Controller& c, double renderChunk, uint32_t rate);
    void PlaceReadPosition(Controller& c);
    double Target(const Controller& c) const;
    double VirtualFill(const Controller& c, int64_t r) const;
    void ClassifyUnderrun(Controller& c);
    void Produce(Controller& c, float* out, UINT32 n);

    // Either audio thread, after leaving its loop.
    void Fail(const std::wstring& reason);

    // UI thread helpers.
    void StopThreads(DWORD giveUpMs);
    HRESULT WaitForThread(HANDLE ready, HANDLE thread, const std::atomic<HRESULT>& result, const wchar_t* what);
    HRESULT PrepareRing(uint32_t rate);

    const HWND notify;

    // UI thread state.
    std::wstring inputId;     // non-empty while started (see InputId() for failures)
    std::wstring endpointId;  // capture endpoint; read by the capture thread while it runs
    float volume = 1.0f;
    bool muted = false;
    UniqueHandle captureThread;
    UniqueHandle renderThread;
    UniqueHandle stopEvent;     // manual reset: both audio threads leave
    UniqueHandle rebuildEvent;  // auto reset: the render side reopens the default output
    UniqueHandle captureGo;     // auto reset: the capture stream may start
    UniqueHandle captureReady;  // auto reset: capture handshake, result in captureResult
    UniqueHandle renderReady;   // auto reset: render handshake, result in renderResult
    std::atomic<HRESULT> captureResult{E_UNEXPECTED};
    std::atomic<HRESULT> renderResult{E_UNEXPECTED};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> failed{false};
    // An audio thread did not finish in time (stuck in a driver call) and still uses this object:
    // it is never started or destroyed again (AudioPassthrough::RetireIfAbandoned).
    bool abandoned = false;
    uint64_t loggedUnderruns = 0, loggedGaps = 0, loggedOverruns = 0, loggedResyncs = 0;
    ULONGLONG lastHealthLog = 0;

    // Ring of interleaved stereo frames, sized by the UI thread while both audio threads are idle.
    std::unique_ptr<float[]> ring;
    uint32_t ringFrames = 0;
    uint32_t ringMask = 0;
    std::atomic<int64_t> wpos{0};
    std::atomic<int64_t> rpos{0};
    // Seqlock-published (write position, QPC of that write), used to extrapolate the fill to "now"
    // so the controller does not see the sawtooth of whole capture packets arriving.
    std::atomic<uint32_t> wseq{0};
    std::atomic<int64_t> wposStamp{0};
    std::atomic<int64_t> wStampQpc{0};
    uint64_t renderOverrunMark = 0;  // render thread only: overruns seen when the read position was last placed

    // Stream facts published by the audio threads for the controller and Status().
    std::atomic<uint32_t> streamRate{0};    // capture rate == render stream rate
    std::atomic<uint32_t> captureChunk{480};  // capture period in frames
    std::atomic<int64_t> captureLatencyHns{0};
    std::atomic<int64_t> renderLatencyHns{0};
    std::atomic<uint32_t> renderPadding{0};

    std::atomic<float> gain{1.0f};
    // Cumulative over this object's lifetime, like the macOS core.
    std::atomic<uint64_t> underruns{0}, inputGaps{0}, overruns{0}, resyncs{0};
    std::atomic<uint64_t> captureCallbacks{0}, renderCallbacks{0}, renderRebuilds{0};
    std::atomic<double> pubFill{0}, pubPpm{0};
    std::atomic<uint32_t> pubTarget{0};
    std::atomic<bool> pubRunning{false};
};

AudioPassthrough::Impl::Impl(HWND notifyWindow) : notify(notifyWindow) {
    stopEvent.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    rebuildEvent.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    captureGo.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    captureReady.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    renderReady.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!stopEvent || !rebuildEvent || !captureGo || !captureReady || !renderReady) {
        Log(L"Audio: CreateEvent failed (error %lu)", static_cast<unsigned long>(GetLastError()));
    }
}

// MARK: UI thread

HRESULT AudioPassthrough::Impl::Start(const std::wstring& id) {
    Stop();
    if (abandoned) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (id.empty()) return E_INVALIDARG;
    if (!stopEvent || !rebuildEvent || !captureGo || !captureReady || !renderReady) {
        return LogIfFailed(E_OUTOFMEMORY, L"Audio: start without events");
    }
    Log(L"Audio: starting, capture endpoint %ls", id.c_str());
    endpointId = id;
    stopRequested = false;
    failed = false;
    captureResult = E_UNEXPECTED;
    renderResult = E_UNEXPECTED;
    for (HANDLE event : {stopEvent.Get(), rebuildEvent.Get(), captureGo.Get(), captureReady.Get(), renderReady.Get()}) {
        ResetEvent(event);
    }

    // 1. The capture side opens first: the render stream is created at its sample rate.
    captureThread.Reset(CreateThread(nullptr, 0, &Impl::CaptureProc, this, 0, nullptr));
    HRESULT hr = captureThread ? WaitForThread(captureReady.Get(), captureThread.Get(), captureResult, L"capture open")
                               : LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Audio: CreateThread(capture)");
    if (SUCCEEDED(hr)) hr = PrepareRing(streamRate.load());

    // 2. Output next, so it is already priming silence when the first input arrives.
    if (SUCCEEDED(hr)) {
        renderThread.Reset(CreateThread(nullptr, 0, &Impl::RenderProc, this, 0, nullptr));
        hr = renderThread ? WaitForThread(renderReady.Get(), renderThread.Get(), renderResult, L"render open")
                          : LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Audio: CreateThread(render)");
    }

    // 3. Let the capture stream run.
    if (SUCCEEDED(hr)) {
        captureResult = E_UNEXPECTED;
        SetEvent(captureGo.Get());
        hr = WaitForThread(captureReady.Get(), captureThread.Get(), captureResult, L"capture start");
    }

    if (FAILED(hr)) {
        Log(L"Audio: start failed (%ls)", Hex(hr).c_str());
        // A thread that already missed its handshake is stuck in the driver: do not wait long again.
        StopThreads(hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) ? kStuckWaitMs : kGiveUpWaitMs);
        return hr;
    }
    inputId = id;
    ApplyGain();
    Log(L"Audio: running");
    return S_OK;
}

void AudioPassthrough::Impl::Stop() {
    const bool hadThreads = captureThread || renderThread;
    StopThreads(kGiveUpWaitMs);
    if (hadThreads) {
        Log(L"Audio: stopped (underruns %llu, input gaps %llu, overruns %llu, resyncs %llu, output rebuilds %llu, "
            L"capture packets %llu, render events %llu)",
            static_cast<unsigned long long>(underruns.load()), static_cast<unsigned long long>(inputGaps.load()),
            static_cast<unsigned long long>(overruns.load()), static_cast<unsigned long long>(resyncs.load()),
            static_cast<unsigned long long>(renderRebuilds.load()),
            static_cast<unsigned long long>(captureCallbacks.load()),
            static_cast<unsigned long long>(renderCallbacks.load()));
    }
    inputId.clear();
    failed = false;
}

void AudioPassthrough::Impl::StopThreads(DWORD giveUpMs) {
    if (abandoned) return;
    stopRequested = true;
    if (stopEvent) SetEvent(stopEvent.Get());
    HANDLE threads[2];
    DWORD count = 0;
    for (const UniqueHandle* thread : {&captureThread, &renderThread}) {
        if (*thread) threads[count++] = thread->Get();
    }
    if (count > 0) {
        DWORD wait = WaitForMultipleObjects(count, threads, TRUE, std::min(kSlowWaitMs, giveUpMs));
        if (wait == WAIT_TIMEOUT && giveUpMs > kSlowWaitMs) {
            Log(L"Audio: an audio thread is slow to finish (busy audio driver?); still waiting");
            wait = WaitForMultipleObjects(count, threads, TRUE, giveUpMs - kSlowWaitMs);
        }
        if (wait == WAIT_TIMEOUT) {
            Log(L"Audio: an audio thread is stuck in the audio driver after %lu ms; leaving it behind",
                static_cast<unsigned long>(giveUpMs));
            abandoned = true;
            NoteAbandonedThread();
            return;  // keep every handle, event and buffer: the stuck thread still uses them
        }
        if (wait == WAIT_FAILED) LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Audio: waiting for the audio threads");
    }
    captureThread.Reset();
    renderThread.Reset();
    for (HANDLE event : {stopEvent.Get(), rebuildEvent.Get(), captureGo.Get(), captureReady.Get(), renderReady.Get()}) {
        if (event) ResetEvent(event);
    }
    renderPadding = 0;
    pubRunning = false;
}

HRESULT AudioPassthrough::Impl::WaitForThread(HANDLE ready, HANDLE thread, const std::atomic<HRESULT>& result,
                                              const wchar_t* what) {
    // Waiting on the thread too means a thread that died early cannot hang the UI.
    const HANDLE handles[2] = {ready, thread};
    DWORD wait = WaitForMultipleObjects(2, handles, FALSE, kSlowWaitMs);
    if (wait == WAIT_TIMEOUT) {
        Log(L"Audio: %ls takes longer than %lu ms (busy audio driver?); still waiting", what,
            static_cast<unsigned long>(kSlowWaitMs));
        wait = WaitForMultipleObjects(2, handles, FALSE, kGiveUpWaitMs - kSlowWaitMs);
    }
    if (wait == WAIT_TIMEOUT) {
        Log(L"Audio: %ls did not finish within %lu ms; giving up", what, static_cast<unsigned long>(kGiveUpWaitMs));
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    if (wait == WAIT_OBJECT_0) return result.load();
    if (wait == WAIT_OBJECT_0 + 1) {
        const HRESULT hr = result.load();
        Log(L"Audio: the audio thread ended during %ls", what);
        return FAILED(hr) ? hr : E_UNEXPECTED;
    }
    return LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Audio: WaitForMultipleObjects");
}

HRESULT AudioPassthrough::Impl::PrepareRing(uint32_t rate) {
    if (rate == 0) return LogIfFailed(E_UNEXPECTED, L"Audio: capture reported no sample rate");
    // Power of two of about 0.34 s: plenty above the largest target plus the resync slack.
    uint32_t frames = kMinRingFrames;
    while (frames < kMaxRingFrames && frames < rate * kRingSeconds) frames <<= 1;
    if (frames != ringFrames || !ring) {
        ring.reset(new (std::nothrow) float[size_t(frames) * 2]);
        if (!ring) {
            ringFrames = ringMask = 0;
            return LogIfFailed(E_OUTOFMEMORY, L"Audio: ring allocation");
        }
        ringFrames = frames;
        ringMask = frames - 1;
    }
    std::fill_n(ring.get(), size_t(frames) * 2, 0.0f);
    wpos = 0;
    rpos = 0;
    wseq = 0;
    wposStamp = 0;
    wStampQpc = 0;
    Log(L"Audio: ring %u frames (%.0f ms)", ringFrames, 1000.0 * ringFrames / rate);
    return S_OK;
}

void AudioPassthrough::Impl::OnDefaultOutputChanged() {
    if (inputId.empty() || failed || !renderThread) {
        Log(L"Audio: default output changed (nothing playing)");
        return;
    }
    Log(L"Audio: default output changed; reopening the output side");
    SetEvent(rebuildEvent.Get());
}

std::optional<AudioStatus> AudioPassthrough::Impl::Status() {
    if (inputId.empty() || failed) return std::nullopt;
    const double rate = streamRate.load(std::memory_order_relaxed);
    if (rate <= 0) return std::nullopt;

    const double fill = pubFill.load(std::memory_order_relaxed);
    const double frames = captureChunk.load(std::memory_order_relaxed) + fill + renderPadding.load(std::memory_order_relaxed);
    AudioStatus status;
    status.latencyMs = frames / rate * 1000.0 +
                       HnsToMs(captureLatencyHns.load(std::memory_order_relaxed) + renderLatencyHns.load(std::memory_order_relaxed));
    status.underruns = underruns.load(std::memory_order_relaxed);
    status.inputGaps = inputGaps.load(std::memory_order_relaxed);

    // Logged when a counter moves and once a minute, so a tester's log shows when and how audio broke up.
    const uint64_t overrunCount = overruns.load(std::memory_order_relaxed);
    const uint64_t resyncCount = resyncs.load(std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    if (status.underruns != loggedUnderruns || status.inputGaps != loggedGaps || overrunCount != loggedOverruns ||
        resyncCount != loggedResyncs || now - lastHealthLog >= 60000) {
        Log(L"Audio: latency ~%.0f ms, fill %.0f / target %u frames, %+.0f ppm, %ls; underruns %llu, input gaps %llu, "
            L"overruns %llu, resyncs %llu, output rebuilds %llu",
            status.latencyMs, fill, pubTarget.load(std::memory_order_relaxed), pubPpm.load(std::memory_order_relaxed),
            pubRunning.load(std::memory_order_relaxed) ? L"playing" : L"priming",
            static_cast<unsigned long long>(status.underruns), static_cast<unsigned long long>(status.inputGaps),
            static_cast<unsigned long long>(overrunCount), static_cast<unsigned long long>(resyncCount),
            static_cast<unsigned long long>(renderRebuilds.load(std::memory_order_relaxed)));
        loggedUnderruns = status.underruns;
        loggedGaps = status.inputGaps;
        loggedOverruns = overrunCount;
        loggedResyncs = resyncCount;
        lastHealthLog = now;
    }
    return status;
}

// MARK: Failure (audio threads, off the hot path)

void AudioPassthrough::Impl::Fail(const std::wstring& reason) {
    if (stopRequested) return;        // Stop() is tearing down anyway; nobody needs to hear about it
    if (failed.exchange(true)) return;  // the other side failed first and already reported
    SetEvent(stopEvent.Get());          // never leave one side running into nothing
    Log(L"Audio: stopped on its own: %ls", reason.c_str());
    if (!notify) return;
    std::wstring* message = nullptr;
    try {
        message = new std::wstring(reason);
    } catch (...) {
        return;
    }
    if (!PostMessageW(notify, WM_APP_AUDIO_FAILED, 0, reinterpret_cast<LPARAM>(message))) {
        Log(L"Audio: PostMessage(WM_APP_AUDIO_FAILED) failed (error %lu)", static_cast<unsigned long>(GetLastError()));
        delete message;
    }
}

// MARK: Capture thread

DWORD WINAPI AudioPassthrough::Impl::CaptureProc(void* self) {
    auto* impl = static_cast<Impl*>(self);
    try {
        impl->CaptureMain();
    } catch (...) {
        // Nothing may propagate out of a thread procedure.
        try {
            Log(L"Audio capture: unexpected exception");
            impl->Fail(L"오디오 입력이 중단되었습니다.");
        } catch (...) {
        }
    }
    return 0;
}

void AudioPassthrough::Impl::CaptureMain() {
    AudioThreadScope scope(L"Audio capture");
    HRESULT hr = scope.Result();
    {
        CaptureStream s;
        if (SUCCEEDED(hr)) hr = OpenCapture(s);
        captureResult = hr;
        SetEvent(captureReady.Get());
        if (FAILED(hr)) return;

        // Start() opens the render side before letting the capture stream run.
        const HANDLE waits[2] = {stopEvent.Get(), captureGo.Get()};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return;
        hr = LogIfFailed(s.client->Start(), L"Audio capture: IAudioClient::Start");
        captureResult = hr;
        SetEvent(captureReady.Get());
        if (FAILED(hr)) return;

        hr = CaptureLoop(s);
        s.client->Stop();
        if (FAILED(hr)) Log(L"Audio capture: stream ended (%ls)", Hex(hr).c_str());
    }
    if (FAILED(hr)) Fail(CaptureFailureReason(hr));
}

HRESULT AudioPassthrough::Impl::OpenCapture(CaptureStream& s) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = LogIfFailed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
                             L"Audio capture: CoCreateInstance(MMDeviceEnumerator)");
    if (FAILED(hr)) return hr;
    ComPtr<IMMDevice> device;
    hr = LogIfFailed(enumerator->GetDevice(endpointId.c_str(), &device), L"Audio capture: IMMDeviceEnumerator::GetDevice");
    if (FAILED(hr)) return hr;
    DWORD state = 0;
    device->GetState(&state);
    Log(L"Audio capture: endpoint \"%ls\" state 0x%lX", EndpointName(device.Get()).c_str(),
        static_cast<unsigned long>(state));

    hr = LogIfFailed(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(s.client.ReleaseAndGetAddressOf())),
                     L"Audio capture: IMMDevice::Activate(IAudioClient)");
    if (FAILED(hr)) return hr;

    WAVEFORMATEX* rawMix = nullptr;
    hr = LogIfFailed(s.client->GetMixFormat(&rawMix), L"Audio capture: GetMixFormat");
    const UniqueCoMem<WAVEFORMATEX> mix(rawMix);
    if (FAILED(hr)) return hr;
    if (!mix) return LogIfFailed(E_POINTER, L"Audio capture: GetMixFormat returned no format");
    Log(L"Audio capture: mix format %ls", DescribeFormat(*mix).c_str());
    const std::optional<CaptureFormat> format = ParseCaptureFormat(*mix);
    if (!format) return LogIfFailed(AUDCLNT_E_UNSUPPORTED_FORMAT, L"Audio capture: unsupported mix format");
    s.format = *format;

    REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
    if (FAILED(LogIfFailed(s.client->GetDevicePeriod(&defaultPeriod, &minimumPeriod), L"Audio capture: GetDevicePeriod")) ||
        defaultPeriod <= 0) {
        defaultPeriod = kFallbackPeriodHns;
    }
    // Shared mode in the mix format: no conversion in the engine. The buffer only adds room for a
    // late wake-up; packets still arrive every period and are drained at once.
    hr = LogIfFailed(s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                          std::max(kCaptureBufferHns, 2 * defaultPeriod), 0, mix.get(), nullptr),
                     L"Audio capture: IAudioClient::Initialize");
    if (FAILED(hr)) return hr;
    s.event.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!s.event) return LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Audio capture: CreateEvent");
    hr = LogIfFailed(s.client->SetEventHandle(s.event.Get()), L"Audio capture: SetEventHandle");
    if (FAILED(hr)) return hr;
    hr = LogIfFailed(s.client->GetBufferSize(&s.bufferFrames), L"Audio capture: GetBufferSize");
    if (FAILED(hr)) return hr;
    hr = LogIfFailed(s.client->GetService(IID_PPV_ARGS(&s.capture)), L"Audio capture: GetService(IAudioCaptureClient)");
    if (FAILED(hr)) return hr;
    OptOutOfDucking(s.client.Get(), L"Audio capture");
    REFERENCE_TIME latency = 0;
    LogIfFailed(s.client->GetStreamLatency(&latency), L"Audio capture: GetStreamLatency");

    try {
        s.scratch.assign(size_t(std::max<UINT32>(s.bufferFrames, 1)) * 2, 0.0f);
    } catch (...) {
        return LogIfFailed(E_OUTOFMEMORY, L"Audio capture: scratch allocation");
    }

    const auto chunk = static_cast<uint32_t>(std::ceil(double(defaultPeriod) * format->rate / kHnsPerSecond));
    streamRate = format->rate;
    captureChunk = std::max<uint32_t>(chunk, 1);
    captureLatencyHns = latency;
    Log(L"Audio capture: %u Hz, buffer %u frames, period %.2f ms (%u frames, minimum %.2f ms), stream latency %.2f ms",
        format->rate, s.bufferFrames, HnsToMs(defaultPeriod), captureChunk.load(), HnsToMs(minimumPeriod),
        HnsToMs(latency));
    return S_OK;
}

HRESULT AudioPassthrough::Impl::CaptureLoop(CaptureStream& s) {
    const HANDLE waits[2] = {stopEvent.Get(), s.event.Get()};
    const double hnsPerFrame = kHnsPerSecond / s.format.rate;
    const auto scratchFrames = static_cast<UINT32>(s.scratch.size() / 2);
    float* const scratch = s.scratch.data();
    bool firstPacket = true;
    bool haveExpected = false;
    UINT64 expectedQpc = 0;  // 100-ns time at which the previous packet ended

    for (;;) {
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, kPollMs);
        if (wait == WAIT_OBJECT_0) return S_OK;
        if (wait == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());

        // Drain every packet. Also on a timeout: GetNextPacketSize is what reports a vanished device
        // when the engine stops signalling.
        for (;;) {
            UINT32 packetFrames = 0;
            HRESULT hr = s.capture->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) return hr;
            if (packetFrames == 0) break;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0, qpcPosition = 0;
            hr = s.capture->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition);
            if (FAILED(hr)) return hr;
            if (hr == AUDCLNT_S_BUFFER_EMPTY) break;

            // An input gap: the engine flagged lost data, or this packet was captured noticeably
            // later than the previous one ended (the device stopped delivering for a while). The
            // first packet after Start is often flagged and has nothing to compare with.
            bool gap = !firstPacket && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0 && qpcPosition != 0) {
                if (haveExpected) {
                    const auto late = static_cast<double>(static_cast<int64_t>(qpcPosition - expectedQpc));
                    if (late > frames * hnsPerFrame) gap = true;
                }
                expectedQpc = qpcPosition + static_cast<UINT64>(frames * hnsPerFrame + 0.5);
                haveExpected = true;
            } else {
                haveExpected = false;
            }
            firstPacket = false;
            // Counted before the write: the render side reads it after seeing the new data.
            if (gap) inputGaps.fetch_add(1, std::memory_order_relaxed);

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data;
            for (UINT32 done = 0; done < frames;) {
                const UINT32 n = std::min(frames - done, scratchFrames);
                if (silent) {
                    std::fill_n(scratch, size_t(n) * 2, 0.0f);
                } else {
                    ConvertToStereo(data + size_t(done) * s.format.blockAlign, n, s.format, scratch);
                }
                RingWrite(scratch, n);
                done += n;
            }
            hr = s.capture->ReleaseBuffer(frames);
            if (FAILED(hr)) return hr;
            captureCallbacks.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void AudioPassthrough::Impl::RingWrite(const float* src, UINT32 n) {
    const int64_t w = wpos.load(std::memory_order_relaxed);
    const int64_t r = rpos.load(std::memory_order_acquire);
    if (static_cast<uint64_t>(w - r) + n > ringFrames) {
        // The render side is not reading (output being rebuilt): drop this chunk; it resyncs later.
        overruns.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto offset = static_cast<uint32_t>(w & ringMask);
    const uint32_t first = std::min(n, ringFrames - offset);
    float* const base = ring.get();
    std::memcpy(base + size_t(offset) * 2, src, size_t(first) * 2 * sizeof(float));
    std::memcpy(base, src + size_t(first) * 2, size_t(n - first) * 2 * sizeof(float));
    wpos.store(w + n, std::memory_order_release);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const uint32_t q = wseq.load(std::memory_order_relaxed);
    wseq.store(q + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    wposStamp.store(w + n, std::memory_order_relaxed);
    wStampQpc.store(now.QuadPart, std::memory_order_relaxed);
    wseq.store(q + 2, std::memory_order_release);
}

// MARK: Render thread

DWORD WINAPI AudioPassthrough::Impl::RenderProc(void* self) {
    auto* impl = static_cast<Impl*>(self);
    try {
        impl->RenderMain();
    } catch (...) {
        try {
            Log(L"Audio render: unexpected exception");
            impl->Fail(L"오디오 출력이 중단되었습니다.");
        } catch (...) {
        }
    }
    return 0;
}

void AudioPassthrough::Impl::RenderMain() {
    AudioThreadScope scope(L"Audio render");
    HRESULT hr = scope.Result();
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (SUCCEEDED(hr)) {
            hr = LogIfFailed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
                             L"Audio render: CoCreateInstance(MMDeviceEnumerator)");
        }
        RenderStream s;
        Controller c;
        renderOverrunMark = overruns.load(std::memory_order_relaxed);
        if (SUCCEEDED(hr)) hr = OpenRender(enumerator.Get(), s, c);
        renderResult = hr;
        SetEvent(renderReady.Get());
        if (FAILED(hr)) return;

        hr = RenderLoop(enumerator.Get(), s, c);
        s.Close();
        if (FAILED(hr)) Log(L"Audio render: stream ended (%ls)", Hex(hr).c_str());
    }
    if (FAILED(hr)) Fail(RenderFailureReason(hr));
}

HRESULT AudioPassthrough::Impl::OpenRender(IMMDeviceEnumerator* enumerator, RenderStream& s, Controller& c) {
    s.Close();
    ComPtr<IMMDevice> device;
    HRESULT hr = LogIfFailed(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device),
                             L"Audio render: GetDefaultAudioEndpoint(eRender, eConsole)");
    if (FAILED(hr)) return hr;
    s.endpointId = EndpointId(device.Get());
    Log(L"Audio render: default output \"%ls\" (%ls)", EndpointName(device.Get()).c_str(), s.endpointId.c_str());

    hr = LogIfFailed(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(s.client.ReleaseAndGetAddressOf())),
                     L"Audio render: IMMDevice::Activate(IAudioClient)");
    if (FAILED(hr)) return hr;
    WAVEFORMATEX* rawMix = nullptr;
    if (SUCCEEDED(LogIfFailed(s.client->GetMixFormat(&rawMix), L"Audio render: GetMixFormat")) && rawMix) {
        Log(L"Audio render: mix format %ls", DescribeFormat(*rawMix).c_str());
    }
    CoTaskMemFree(rawMix);

    // Float stereo at the capture rate; the engine converts rate, channels and sample format to the
    // device's mix format, so this works for any output without a resampler of our own.
    const uint32_t rate = streamRate.load();
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = 2;
    format.Format.nSamplesPerSec = rate;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD>(2 * sizeof(float));
    format.Format.nAvgBytesPerSec = rate * format.Format.nBlockAlign;
    format.Format.cbSize = static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = WaveSubtype(WAVE_FORMAT_IEEE_FLOAT);
    const DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                              AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = LogIfFailed(s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, kRenderBufferHns, 0, &format.Format, nullptr),
                     L"Audio render: IAudioClient::Initialize (float stereo, autoconvert)");
    if (FAILED(hr)) return hr;
    s.event.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!s.event) return LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Audio render: CreateEvent");
    hr = LogIfFailed(s.client->SetEventHandle(s.event.Get()), L"Audio render: SetEventHandle");
    if (FAILED(hr)) return hr;
    hr = LogIfFailed(s.client->GetBufferSize(&s.bufferFrames), L"Audio render: GetBufferSize");
    if (FAILED(hr)) return hr;
    hr = LogIfFailed(s.client->GetService(IID_PPV_ARGS(&s.render)), L"Audio render: GetService(IAudioRenderClient)");
    if (FAILED(hr)) return hr;
    OptOutOfDucking(s.client.Get(), L"Audio render");
    REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
    if (FAILED(LogIfFailed(s.client->GetDevicePeriod(&defaultPeriod, &minimumPeriod), L"Audio render: GetDevicePeriod")) ||
        defaultPeriod <= 0) {
        defaultPeriod = kFallbackPeriodHns;
    }
    REFERENCE_TIME latency = 0;
    LogIfFailed(s.client->GetStreamLatency(&latency), L"Audio render: GetStreamLatency");
    renderLatencyHns = latency;

    // Prime the endpoint buffer with silence so the first period cannot glitch.
    BYTE* data = nullptr;
    hr = LogIfFailed(s.render->GetBuffer(s.bufferFrames, &data), L"Audio render: GetBuffer (prime)");
    if (SUCCEEDED(hr)) {
        hr = LogIfFailed(s.render->ReleaseBuffer(s.bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT), L"Audio render: ReleaseBuffer (prime)");
    }
    if (FAILED(hr)) return hr;
    renderPadding = s.bufferFrames;

    ResetController(c, std::ceil(double(defaultPeriod) * rate / kHnsPerSecond), rate);
    hr = LogIfFailed(s.client->Start(), L"Audio render: IAudioClient::Start");
    if (FAILED(hr)) return hr;
    s.started = true;
    Log(L"Audio render: %u Hz float stereo, buffer %u frames (%.2f ms), period %.2f ms, stream latency %.2f ms, "
        L"target %.0f frames",
        rate, s.bufferFrames, 1000.0 * s.bufferFrames / rate, HnsToMs(defaultPeriod), HnsToMs(latency), Target(c));
    return S_OK;
}

void AudioPassthrough::Impl::ResetController(Controller& c, double renderChunk, uint32_t rate) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    // A margin learned on another device or under a load burst does not carry over.
    c = Controller{};
    c.rate = rate;
    c.renderChunk = std::max(renderChunk, 1.0);
    c.framesPerQpcTick = double(rate) / double(frequency.QuadPart);
    c.minHeadroom = std::numeric_limits<double>::infinity();
    // Deferred to the first event: the endpoint buffer was just primed with silence, so the stream
    // reads nothing for about a period, and the ring keeps filling meanwhile.
    c.placeReadPosition = true;
}

/// Render thread, first event after (re)opening the output. Only this thread moves rpos, so it may
/// jump. Resume `target` frames behind the newest audio (counting what the capture side holds in
/// flight, as the controller does), so an output switch adds neither latency nor a gap and the
/// controller starts without an error to work off. If the ring overflowed while nobody read it, the
/// newest frames are stale (later packets were dropped): start at the newest and prime with
/// silence instead. Never move back over audio that was already played.
void AudioPassthrough::Impl::PlaceReadPosition(Controller& c) {
    c.placeReadPosition = false;
    const int64_t w = wpos.load(std::memory_order_acquire);
    const int64_t r = rpos.load(std::memory_order_relaxed);
    const uint64_t overrunCount = overruns.load(std::memory_order_relaxed);
    const double inFlight = std::max(VirtualFill(c, w), 0.0);
    int64_t start = overrunCount != renderOverrunMark ? w : w - static_cast<int64_t>(Target(c) - inFlight);
    renderOverrunMark = overrunCount;
    if (start < r) start = r;
    rpos.store(start, std::memory_order_release);
}

HRESULT AudioPassthrough::Impl::RebuildRender(IMMDeviceEnumerator* enumerator, RenderStream& s, Controller& c, bool lost) {
    if (!lost && s.client) {
        // The watcher and our own invalidation handling can both ask: keep a stream that already
        // plays on the default output.
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
            EndpointId(device.Get()) == s.endpointId) {
            Log(L"Audio render: already on the default output");
            return S_OK;
        }
    }
    s.Close();
    renderPadding = 0;
    HRESULT hr = E_FAIL;
    for (const DWORD delay : kRenderRetryDelaysMs) {
        // Right after an unplug the old endpoint can still be the default for a moment. A new
        // rebuild request just retries at once.
        const HANDLE waits[2] = {stopEvent.Get(), rebuildEvent.Get()};
        if (WaitForMultipleObjects(2, waits, FALSE, delay) == WAIT_OBJECT_0) return S_FALSE;
        hr = OpenRender(enumerator, s, c);
        if (SUCCEEDED(hr)) {
            renderRebuilds.fetch_add(1, std::memory_order_relaxed);
            return S_OK;
        }
        s.Close();
    }
    Log(L"Audio render: could not reopen an output (%ls)", Hex(hr).c_str());
    return hr;
}

HRESULT AudioPassthrough::Impl::RenderLoop(IMMDeviceEnumerator* enumerator, RenderStream& s, Controller& c) {
    int quickFailures = 0;
    for (;;) {
        const HANDLE waits[3] = {stopEvent.Get(), rebuildEvent.Get(), s.event.Get()};
        const DWORD wait = WaitForMultipleObjects(s.event ? 3 : 2, waits, FALSE, kPollMs);
        if (wait == WAIT_OBJECT_0) return S_OK;
        if (wait == WAIT_FAILED) return LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Audio render: WaitForMultipleObjects");

        HRESULT hr = S_OK;
        if (wait == WAIT_OBJECT_0 + 1) {
            hr = RebuildRender(enumerator, s, c, false);
        } else {
            // The engine's event, or a timeout poll that notices a stream that died silently.
            hr = s.client ? RenderOnce(s, c) : AUDCLNT_E_NOT_INITIALIZED;
            if (FAILED(hr)) {
                // The output went away or was reconfigured: follow the default output like the
                // macOS app does, and give up only if it keeps dying right after opening.
                quickFailures = s.callbacks < kHealthyRenderCallbacks ? quickFailures + 1 : 1;
                if (quickFailures > kMaxQuickRenderFailures) return hr;
                Log(L"Audio render: stream lost (%ls); reopening the default output", Hex(hr).c_str());
                hr = RebuildRender(enumerator, s, c, true);
            }
        }
        if (hr == S_FALSE) return S_OK;  // Stop() arrived during a rebuild
        if (FAILED(hr)) return hr;
    }
}

HRESULT AudioPassthrough::Impl::RenderOnce(RenderStream& s, Controller& c) {
    UINT32 padding = 0;
    HRESULT hr = s.client->GetCurrentPadding(&padding);
    if (FAILED(hr)) return hr;
    renderPadding.store(padding, std::memory_order_relaxed);
    const UINT32 frames = padding < s.bufferFrames ? s.bufferFrames - padding : 0;
    if (frames == 0) return S_OK;
    BYTE* data = nullptr;
    hr = s.render->GetBuffer(frames, &data);
    if (FAILED(hr)) return hr;
    Produce(c, reinterpret_cast<float*>(data), frames);
    hr = s.render->ReleaseBuffer(frames, 0);
    if (FAILED(hr)) return hr;
    s.callbacks++;
    renderCallbacks.fetch_add(1, std::memory_order_relaxed);
    return S_OK;
}

double AudioPassthrough::Impl::Target(const Controller& c) const {
    return double(captureChunk.load(std::memory_order_relaxed)) + c.renderChunk + kMarginFrames + c.extraTarget;
}

/// Fill with the write position extrapolated to "now", capped at one capture period.
double AudioPassthrough::Impl::VirtualFill(const Controller& c, int64_t r) const {
    const double maxExtra = captureChunk.load(std::memory_order_relaxed);
    for (int tries = 0; tries < 4; tries++) {
        const uint32_t q0 = wseq.load(std::memory_order_acquire);
        if (q0 & 1) continue;
        const int64_t w = wposStamp.load(std::memory_order_relaxed);
        const int64_t stamp = wStampQpc.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (wseq.load(std::memory_order_relaxed) != q0) continue;
        double extra = 0;
        if (stamp != 0) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            extra = std::clamp(double(now.QuadPart - stamp) * c.framesPerQpcTick, 0.0, maxExtra);
        }
        return double(w - r) + extra;
    }
    return double(wpos.load(std::memory_order_acquire) - r);
}

/// Called when audio flows again after the ring ran dry. If the input reported a gap in the
/// meantime, the device itself went quiet (a bigger buffer would not have helped), so the latency
/// is left alone. Otherwise our margin was too small for the two threads' jitter: raise the target.
void AudioPassthrough::Impl::ClassifyUnderrun(Controller& c) {
    c.underrunPending = false;
    if (inputGaps.load(std::memory_order_relaxed) != c.gapsAtUnderrun) return;
    underruns.fetch_add(1, std::memory_order_relaxed);
    c.extraTarget = std::min(c.extraTarget + kTargetStepFrames, kTargetMaxExtra);
    // Start a fresh decay window for the new target.
    c.quietSeconds = 0;
    c.minHeadroom = std::numeric_limits<double>::infinity();
}

/// Writes `n` interleaved stereo frames for the endpoint. Render thread hot path: no locks,
/// allocation or logging.
void AudioPassthrough::Impl::Produce(Controller& c, float* out, UINT32 n) {
    if (c.placeReadPosition) PlaceReadPosition(c);
    const int64_t w = wpos.load(std::memory_order_acquire);
    int64_t r = rpos.load(std::memory_order_relaxed);
    const double fill = double(w - r);
    double target = Target(c);
    const double dt = double(n) / c.rate;

    if (!c.running) {
        // Resume on the extrapolated fill, the quantity the controller steers. With 10 ms capture
        // packets the raw fill can sit up to a whole packet above it, and starting there makes the
        // controller drain that excess, wind up its integral and overshoot into an underrun (seen in
        // simulation; the macOS core's 128-frame IO buffers hide this). Resuming needs new input
        // either way: the raw fill must be about a render period plus the margin.
        const double virtualFill = VirtualFill(c, r);
        if (virtualFill >= target && c.underrunPending) {
            // Decide before resuming so a raised target takes effect now.
            ClassifyUnderrun(c);
            target = Target(c);
        }
        if (virtualFill >= target) {
            c.running = true;
            c.avgFill = virtualFill;
            c.frac = 0;
        }
    } else if (fill > target + kResyncSlackFrames) {
        // Backlog after a stall: jump back to the target instead of slowly draining it at +1000 ppm.
        r = w - static_cast<int64_t>(target);
        rpos.store(r, std::memory_order_release);
        c.avgFill = target;
        c.integ = 0;
        c.frac = 0;
        resyncs.fetch_add(1, std::memory_order_relaxed);
    }

    const double a = std::min(dt / kFillEmaSeconds, 1.0);
    c.avgFill += (VirtualFill(c, r) - c.avgFill) * a;
    const double e = c.avgFill - target;
    if (c.running) {
        // Anti-windup: the integral alone can never ask for more than the rate limit.
        const double limit = kMaxPpm / kKiPpmPerFrameS;
        c.integ = std::clamp(c.integ + e * dt, -limit, limit);

        // Give back some of the learned margin, but only if it was not needed: otherwise this would
        // step down until the next dropout, forever.
        c.quietSeconds += dt;
        if (c.quietSeconds >= kTargetDecaySeconds) {
            if (c.extraTarget > 0 && c.minHeadroom > kTargetDecayFrames + kDecayHeadroomFrames) {
                c.extraTarget = c.extraTarget > kTargetDecayFrames ? c.extraTarget - kTargetDecayFrames : 0;
            }
            c.quietSeconds = 0;
            c.minHeadroom = std::numeric_limits<double>::infinity();
        }
    }
    const double ppm = std::clamp(kKpPpmPerFrame * e + kKiPpmPerFrameS * c.integ, -kMaxPpm, kMaxPpm);
    // A step above 1 consumes input faster, draining the ring when the capture clock runs fast.
    const double step = 1.0 + ppm * 1e-6;

    bool played = false;
    if (c.running) {
        // Linear interpolation reads frames i and i + 1; `last` uses the same expression as the loop
        // below so rounding can never make the loop read past what was checked.
        const auto last = static_cast<int64_t>(c.frac + double(n - 1) * step);
        const int64_t need = last + 2;
        const int64_t available = w - r;
        if (available < need) {
            // Ran dry: play silence and re-prime to the target; classify once audio flows again.
            c.running = false;
            c.underrunPending = true;
            c.gapsAtUnderrun = inputGaps.load(std::memory_order_relaxed);
        } else {
            const float g = gain.load(std::memory_order_relaxed);
            const float* const src = ring.get();
            const auto mask = static_cast<int64_t>(ringMask);
            for (UINT32 k = 0; k < n; k++) {
                const double pos = c.frac + double(k) * step;
                const int64_t i = std::min(static_cast<int64_t>(pos), last);
                const auto t = static_cast<float>(pos - double(i));
                const size_t i0 = size_t((r + i) & mask) * 2;
                const size_t i1 = size_t((r + i + 1) & mask) * 2;
                out[2 * size_t(k)] = (src[i0] + (src[i1] - src[i0]) * t) * g;
                out[2 * size_t(k) + 1] = (src[i0 + 1] + (src[i1 + 1] - src[i0 + 1]) * t) * g;
            }
            const double end = c.frac + double(n) * step;
            const int64_t advance = std::min(static_cast<int64_t>(end), available);
            c.frac = end - double(advance);
            rpos.store(r + advance, std::memory_order_release);
            c.minHeadroom = std::min(c.minHeadroom, double(available - need));
            played = true;
        }
    }
    if (!played) std::memset(out, 0, size_t(n) * 2 * sizeof(float));

    pubFill.store(c.avgFill, std::memory_order_relaxed);
    pubPpm.store(ppm, std::memory_order_relaxed);
    pubTarget.store(static_cast<uint32_t>(target), std::memory_order_relaxed);
    pubRunning.store(c.running, std::memory_order_relaxed);
}

// MARK: - AudioPassthrough

AudioPassthrough::AudioPassthrough(HWND notify) : impl_(std::make_unique<Impl>(notify)) {}

AudioPassthrough::~AudioPassthrough() {
    impl_->Stop();
    if (impl_->abandoned) (void)impl_.release();  // a stuck audio thread still uses it
}

HRESULT AudioPassthrough::Start(const std::wstring& endpointId) {
    if (GetTickCount64() < retryAfter_) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    const HRESULT hr = impl_->Start(endpointId);
    RetireIfAbandoned();
    return hr;
}

void AudioPassthrough::Stop() {
    impl_->Stop();
    RetireIfAbandoned();
}

void AudioPassthrough::RetireIfAbandoned() {
    if (!impl_->abandoned) return;
    // The stuck thread keeps the old state (deliberately leaked); carry on with fresh state, but
    // only after a pause, so a hung driver cannot freeze the window again every few seconds.
    auto fresh = std::make_unique<Impl>(impl_->notify);
    fresh->volume = impl_->volume;
    fresh->muted = impl_->muted;
    fresh->ApplyGain();
    (void)impl_.release();
    impl_ = std::move(fresh);
    retryAfter_ = GetTickCount64() + kAbandonedRetryPauseMs;
    Log(L"Audio: continuing on fresh state; next start in %llu s",
        static_cast<unsigned long long>(kAbandonedRetryPauseMs / 1000));
}

std::wstring AudioPassthrough::InputId() const {
    // After a failure the audio threads have stopped by themselves; report "not playing" so the
    // owner's periodic check starts it again.
    return impl_->failed ? std::wstring() : impl_->inputId;
}

void AudioPassthrough::SetVolume(float volume) {
    impl_->volume = std::isfinite(volume) ? std::clamp(volume, 0.0f, 1.0f) : 0.0f;
    impl_->ApplyGain();
}

void AudioPassthrough::SetMuted(bool muted) {
    impl_->muted = muted;
    impl_->ApplyGain();
}

std::optional<AudioStatus> AudioPassthrough::Status() const {
    return impl_->Status();
}

void AudioPassthrough::OnDefaultOutputChanged() {
    impl_->OnDefaultOutputChanged();
}

// MARK: - AudioDeviceWatcher

namespace {

/// Receives endpoint notifications on a system worker thread; only logs and posts.
class EndpointNotifier final : public IMMNotificationClient {
public:
    explicit EndpointNotifier(HWND notify) : notify_(notify) {}
    EndpointNotifier(const EndpointNotifier&) = delete;
    EndpointNotifier& operator=(const EndpointNotifier&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = refs_.fetch_sub(1) - 1;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        Log(L"Audio endpoints: %ls -> state 0x%lX", Text(id), static_cast<unsigned long>(state));
        Post(WM_APP_AUDIO_DEVICES_CHANGED);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override {
        Log(L"Audio endpoints: added %ls", Text(id));
        Post(WM_APP_AUDIO_DEVICES_CHANGED);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        Log(L"Audio endpoints: removed %ls", Text(id));
        Post(WM_APP_AUDIO_DEVICES_CHANGED);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        // Only the console render role is the "default output" the passthrough plays to.
        if (flow == eRender && role == eConsole) {
            Log(L"Audio endpoints: default output is now %ls", Text(id));
            Post(WM_APP_DEFAULT_OUTPUT_CHANGED);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    ~EndpointNotifier() = default;

    static const wchar_t* Text(LPCWSTR id) { return id ? id : L"(none)"; }
    void Post(UINT message) const {
        if (notify_) PostMessageW(notify_, message, 0, 0);
    }

    std::atomic<ULONG> refs_{1};
    const HWND notify_;
};

}  // namespace

struct AudioDeviceWatcher::Impl {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<EndpointNotifier> client;
    bool registered = false;

    ~Impl() {
        // The enumerator holds only a raw pointer: unregister before our reference goes away.
        if (registered) {
            LogIfFailed(enumerator->UnregisterEndpointNotificationCallback(client.Get()),
                        L"AudioDeviceWatcher: UnregisterEndpointNotificationCallback");
        }
    }
};

AudioDeviceWatcher::AudioDeviceWatcher(HWND notify) : impl_(std::make_unique<Impl>()) {
    HRESULT hr = LogIfFailed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&impl_->enumerator)),
                             L"AudioDeviceWatcher: CoCreateInstance(MMDeviceEnumerator)");
    if (FAILED(hr)) return;
    impl_->client.Attach(new (std::nothrow) EndpointNotifier(notify));
    if (!impl_->client) {
        LogIfFailed(E_OUTOFMEMORY, L"AudioDeviceWatcher: notification client");
        return;
    }
    hr = LogIfFailed(impl_->enumerator->RegisterEndpointNotificationCallback(impl_->client.Get()),
                     L"AudioDeviceWatcher: RegisterEndpointNotificationCallback");
    impl_->registered = SUCCEEDED(hr);
    if (impl_->registered) Log(L"AudioDeviceWatcher: watching audio endpoints");
}

AudioDeviceWatcher::~AudioDeviceWatcher() = default;
