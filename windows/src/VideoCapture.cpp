// Media Foundation capture: an async source reader hands every frame straight to the sink.
//
// Threads:
//   - Caller (UI) thread: Start / Stop / Restart only queue a request and wake the worker. Start
//     and Stop also cut frame delivery at once (under Shared::delivery), so once they return the
//     sink never sees another frame of the old stream.
//   - Worker thread (MTA): opens and closes readers one request at a time, like the macOS session
//     queue; opening a device can block for a second or more.
//   - MF work-queue threads: ReaderCallback::OnReadSample hands the frame to the sink and asks for
//     the next one right away.
//
// Every opened reader is a "session" with its own callback object. A callback only acts while its
// session id is Shared::liveSession, and it holds Shared through a shared_ptr, so late callbacks
// from a reader that is being (or has been) released are harmless.

#include "VideoCapture.h"

#include <mferror.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <functional>
#include <new>
#include <thread>
#include <utility>

namespace {

constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);

// How long Start waits for the first frame (or the first error) before reporting success anyway.
// A busy device or a camera-privacy denial usually fails on the first read, and the App's
// "could not start" message is clearer than "no signal". A card without HDMI input may deliver
// nothing at all, so this cannot wait for a frame forever.
constexpr DWORD kFirstResultTimeoutMs = 1500;
// Upper bound for OnFlush while closing: a vanished device may never answer.
constexpr DWORD kFlushTimeoutMs = 2000;
constexpr DWORD kWorkerExitTimeoutMs = 5000;  // then the worker is left behind (stuck in the driver)

/// Rounds 59.9400599 to 59.94 so equal rates compare equal (same as the menus).
double RoundRate(double rate) { return std::round(rate * 100.0) / 100.0; }

/// A short English hint for HRESULTs a tester is likely to hit; nullptr if none.
const wchar_t* ExplainHr(HRESULT hr) {
    if (hr == E_ACCESSDENIED)
        return L"ACCESS DENIED: Windows camera privacy is blocking desktop apps. Settings > Privacy & security > "
               L"Camera > \"Let desktop apps access your camera\" (ms-settings:privacy-webcam).";
    if (hr == MF_E_HW_MFT_FAILED_START_STREAMING)
        return L"the device could not start streaming (often: another app such as OBS is using it)";
    if (hr == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED) return L"the device was disconnected";
    if (hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED) return L"another app took over the device";
    if (hr == MF_E_VIDEO_DEVICE_LOCKED) return L"the device is locked by another app";
    if (hr == MF_E_SHUTDOWN) return L"the media source was already shut down";
    if (hr == MF_E_END_OF_STREAM) return L"the device ended the stream (unplugged?)";
    if (hr == MF_E_NOT_INITIALIZED) return L"Media Foundation is not started (MFStartup)";
    if (hr == CO_E_NOTINITIALIZED) return L"COM is not initialised on this thread";
    if (hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_INVALIDTYPE || hr == MF_E_TOPO_CODEC_NOT_FOUND ||
        hr == MF_E_TRANSFORM_TYPE_NOT_SET)
        return L"format not accepted (no decoder or converter for it)";
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ||
        hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) || hr == HRESULT_FROM_WIN32(ERROR_DEV_NOT_EXIST) ||
        hr == HRESULT_FROM_WIN32(ERROR_GEN_FAILURE) || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        return L"device not found (unplugged, or moved to another USB port?)";
    if (hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || hr == HRESULT_FROM_WIN32(ERROR_BUSY))
        return L"the device is in use by another app";
    return nullptr;
}

/// Logs a failed HRESULT plus a hint when one is known; returns hr unchanged.
HRESULT Check(HRESULT hr, const wchar_t* what) {
    if (FAILED(hr)) {
        LogIfFailed(hr, what);
        if (const wchar_t* why = ExplainHr(hr)) Log(L"Video:   -> %ls", why);
    }
    return hr;
}

std::wstring SubtypeName(const GUID& subtype) {
    if (subtype == MFVideoFormat_NV12) return L"NV12";
    if (subtype == MFVideoFormat_YUY2) return L"YUY2";
    if (subtype == MFVideoFormat_MJPG) return L"MJPG";
    // Video subtype GUIDs carry a FOURCC (or a D3DFORMAT number) in Data1.
    const DWORD code = subtype.Data1;
    std::wstring name;
    for (int i = 0; i < 4; ++i) {
        const unsigned c = (code >> (8 * i)) & 0xFF;
        if (c < 0x20 || c > 0x7E) {
            wchar_t number[16];
            swprintf(number, 16, L"0x%08lX", static_cast<unsigned long>(code));
            return number;
        }
        name.push_back(static_cast<wchar_t>(c));
    }
    return name;
}

/// Lower is better; -1 = not something this app can render (the Renderer takes NV12 / YUY2, and
/// the source reader decodes MJPG to one of them).
int SubtypeRank(const GUID& subtype) {
    if (subtype == MFVideoFormat_NV12) return 0;
    if (subtype == MFVideoFormat_YUY2) return 1;
    if (subtype == MFVideoFormat_MJPG) return 2;
    return -1;
}

struct NativeType {
    ComPtr<IMFMediaType> type;
    DWORD index = 0;
    GUID subtype = GUID_NULL;
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 rateNum = 0;
    UINT32 rateDen = 0;
    double rate = 0;  // exact MF_MT_FRAME_RATE
    int rank = -1;

    uint64_t Area() const { return uint64_t(width) * height; }
    bool Usable() const { return rank >= 0 && width > 0 && height > 0; }
};

std::optional<NativeType> DescribeType(IMFMediaType* type, DWORD index) {
    if (!type) return std::nullopt;
    GUID major = GUID_NULL;
    if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video) return std::nullopt;
    NativeType t;
    t.type = type;
    t.index = index;
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &t.subtype))) t.subtype = GUID_NULL;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &t.width, &t.height))) t.width = t.height = 0;
    if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &t.rateNum, &t.rateDen)) && t.rateDen != 0) {
        t.rate = double(t.rateNum) / t.rateDen;
    } else {
        t.rateNum = t.rateDen = 0;
    }
    t.rank = SubtypeRank(t.subtype);
    return t;
}

std::wstring TypeLabel(const NativeType& t) {
    wchar_t text[128];
    swprintf(text, 128, L"%ls %ux%u @ %.3f (%u/%u)", SubtypeName(t.subtype).c_str(), t.width, t.height, t.rate,
             t.rateNum, t.rateDen);
    return text;
}

std::wstring DescribeRequest(const std::optional<SIZE>& size, const std::optional<double>& fps) {
    std::wstring text = size ? std::to_wstring(size->cx) + L"x" + std::to_wstring(size->cy) : L"auto size";
    if (fps) {
        wchar_t rate[32];
        swprintf(rate, 32, L", %.2f fps", *fps);
        text += rate;
    } else {
        text += L", auto fps";
    }
    return text;
}

/// SPEC "format choice": among types whose rate reaches min(requested or 50, best) - 0.5, the
/// largest area (an exact requested size wins when offered); then the closest rate (or the
/// fastest); then NV12 > YUY2 > MJPG.
std::optional<size_t> ChooseFormat(const std::vector<NativeType>& types, std::optional<SIZE> size,
                                   std::optional<double> fps) {
    double fastest = 0;
    bool any = false;
    for (const NativeType& t : types) {
        if (!t.Usable()) continue;
        any = true;
        fastest = std::max(fastest, t.rate);
    }
    if (!any) return std::nullopt;

    // For "largest", only sizes that reach the wanted rate (or 50+ fps): a card offering 4K30
    // and 1080p60 should play 1080p60.
    const double minimumRate = std::min(fps.value_or(50.0), fastest) - 0.5;
    uint64_t largest = 0;
    for (const NativeType& t : types) {
        if (t.Usable() && t.rate >= minimumRate) largest = std::max(largest, t.Area());
    }
    const bool wantsSize = size && size->cx > 0 && size->cy > 0;
    auto isExact = [&](const NativeType& t) {
        return wantsSize && t.width == UINT32(size->cx) && t.height == UINT32(size->cy);
    };
    const bool exactExists = std::any_of(types.begin(), types.end(),
                                         [&](const NativeType& t) { return t.Usable() && isExact(t); });

    std::optional<size_t> best;
    std::pair<double, int> bestScore{0.0, 0};
    for (size_t i = 0; i < types.size(); ++i) {
        const NativeType& t = types[i];
        if (!t.Usable()) continue;
        if (exactExists ? !isExact(t) : (t.rate < minimumRate || t.Area() != largest)) continue;
        // Rates compare at the menus' 0.01 precision, so a format reporting 60.0002 does not beat a
        // better pixel format reporting 60/1.
        const double rateScore = RoundRate(fps ? -std::fabs(t.rate - *fps) : t.rate);
        const std::pair<double, int> score{rateScore, -t.rank};
        if (!best || score > bestScore) {
            best = i;
            bestScore = score;
        }
    }
    return best;
}

HRESULT CreateDeviceSource(const std::wstring& link, ComPtr<IMFMediaSource>& source) {
    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(hr)) {
        hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    }
    if (SUCCEEDED(hr)) hr = attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
    if (SUCCEEDED(hr)) hr = MFCreateDeviceSource(attributes.Get(), &source);
    return hr;
}

/// Native types of the source's first video stream, read from its presentation descriptor
/// (nothing is started). Same order as IMFSourceReader::GetNativeMediaType.
HRESULT EnumerateSourceTypes(IMFMediaSource* source, std::vector<NativeType>& types) {
    ComPtr<IMFPresentationDescriptor> descriptor;
    HRESULT hr = source->CreatePresentationDescriptor(&descriptor);
    if (FAILED(hr)) return hr;
    DWORD streams = 0;
    hr = descriptor->GetStreamDescriptorCount(&streams);
    if (FAILED(hr)) return hr;
    for (DWORD s = 0; s < streams; ++s) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> stream;
        ComPtr<IMFMediaTypeHandler> handler;
        GUID major = GUID_NULL;
        if (FAILED(descriptor->GetStreamDescriptorByIndex(s, &selected, &stream)) ||
            FAILED(stream->GetMediaTypeHandler(&handler)) || FAILED(handler->GetMajorType(&major)) ||
            major != MFMediaType_Video) {
            continue;
        }
        DWORD count = 0;
        hr = handler->GetMediaTypeCount(&count);
        if (FAILED(hr)) return hr;
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IMFMediaType> type;
            if (FAILED(Check(handler->GetMediaTypeByIndex(i, &type), L"Video: GetMediaTypeByIndex"))) continue;
            if (auto t = DescribeType(type.Get(), i)) types.push_back(std::move(*t));
        }
        return types.empty() ? MF_E_INVALIDMEDIATYPE : S_OK;
    }
    return MF_E_INVALIDSTREAMNUMBER;  // no video stream at all
}

HRESULT EnumerateReaderTypes(IMFSourceReader* reader, std::vector<NativeType>& types) {
    for (DWORD i = 0; i < 4096; ++i) {
        ComPtr<IMFMediaType> type;
        const HRESULT hr = reader->GetNativeMediaType(kVideoStream, i, &type);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr)) {
            Check(hr, L"Video: GetNativeMediaType");
            break;
        }
        if (auto t = DescribeType(type.Get(), i)) types.push_back(std::move(*t));
    }
    return types.empty() ? MF_E_INVALIDMEDIATYPE : S_OK;
}

/// Selects `native` (this is what sets the device's own format) and, for MJPG, asks the reader
/// for decoded frames.
HRESULT ConfigureOutput(IMFSourceReader* reader, const NativeType& native) {
    HRESULT hr = Check(reader->SetCurrentMediaType(kVideoStream, nullptr, native.type.Get()),
                       L"Video: SetCurrentMediaType (native)");
    if (FAILED(hr)) return hr;
    if (native.subtype == MFVideoFormat_NV12 || native.subtype == MFVideoFormat_YUY2) return S_OK;

    // The reader inserts the MJPEG decoder. NV12 is preferred; YUY2 is the decoder's classic output
    // (plain reader video processing only converts to RGB32, so it cannot bridge YUY2 -> NV12).
    // Size and rate keep the reader on the chosen native type; the bare forms are a last resort
    // for decoders that reject extra attributes.
    struct Attempt {
        const GUID* subtype;
        bool detailed;
        const wchar_t* label;
    };
    const Attempt attempts[] = {
        {&MFVideoFormat_NV12, true, L"Video: SetCurrentMediaType (MJPG -> NV12)"},
        {&MFVideoFormat_YUY2, true, L"Video: SetCurrentMediaType (MJPG -> YUY2)"},
        {&MFVideoFormat_NV12, false, L"Video: SetCurrentMediaType (MJPG -> NV12, bare)"},
        {&MFVideoFormat_YUY2, false, L"Video: SetCurrentMediaType (MJPG -> YUY2, bare)"},
    };
    for (const Attempt& attempt : attempts) {
        ComPtr<IMFMediaType> output;
        hr = MFCreateMediaType(&output);
        if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_SUBTYPE, *attempt.subtype);
        if (SUCCEEDED(hr) && attempt.detailed) {
            hr = MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, native.width, native.height);
            // The native type's own ratio, never a rounded one.
            if (SUCCEEDED(hr) && native.rateDen != 0) {
                hr = MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, native.rateNum, native.rateDen);
            }
            if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            UINT32 parNum = 1, parDen = 1;
            if (FAILED(MFGetAttributeRatio(native.type.Get(), MF_MT_PIXEL_ASPECT_RATIO, &parNum, &parDen)) ||
                parNum == 0 || parDen == 0) {
                parNum = parDen = 1;
            }
            if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, parNum, parDen);
        }
        if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(kVideoStream, nullptr, output.Get());
        if (SUCCEEDED(hr)) {
            Log(L"Video: MJPG is decoded to %ls by the source reader", SubtypeName(*attempt.subtype).c_str());
            return S_OK;
        }
        Check(hr, attempt.label);
    }
    return hr;
}

/// What the reader actually delivers. Only NV12 / YUY2 can be rendered.
HRESULT ReadMode(IMFMediaType* type, double fallbackFps, VideoMode& mode) {
    VideoMode m;
    HRESULT hr = type->GetGUID(MF_MT_SUBTYPE, &m.subtype);
    if (SUCCEEDED(hr)) hr = MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &m.width, &m.height);
    if (FAILED(hr)) return hr;
    UINT32 num = 0, den = 0;
    m.fps = SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den)) && den != 0 ? double(num) / den
                                                                                             : fallbackFps;
    if ((m.subtype != MFVideoFormat_NV12 && m.subtype != MFVideoFormat_YUY2) || m.width == 0 || m.height == 0) {
        Log(L"Video: delivered type %ls %ux%u cannot be rendered (needs NV12 or YUY2)",
            SubtypeName(m.subtype).c_str(), m.width, m.height);
        return MF_E_INVALIDMEDIATYPE;
    }
    mode = m;
    return S_OK;
}

void LogDeliveredType(IMFMediaType* type, const VideoMode& mode, const wchar_t* what) {
    // The renderer assumes BT.709 limited range; what the device claims helps explain odd colours.
    UINT32 strideBits = 0, range = 0, matrix = 0;
    const INT32 stride =
        SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideBits)) ? static_cast<INT32>(strideBits) : 0;
    if (FAILED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range))) range = 0;
    if (FAILED(type->GetUINT32(MF_MT_YUV_MATRIX, &matrix))) matrix = 0;
    Log(L"Video: %ls %ls %ux%u @ %.3f fps (default stride %d, nominal range %u, YUV matrix %u)", what,
        SubtypeName(mode.subtype).c_str(), mode.width, mode.height, mode.fps, static_cast<int>(stride), range,
        matrix);
}

/// Distinct usable sizes (largest first) and their rounded rates (fastest first).
VideoCaps CapsFromTypes(const std::vector<NativeType>& types) {
    VideoCaps caps;
    for (const NativeType& t : types) {
        if (!t.Usable()) continue;
        const uint64_t key = (uint64_t(t.width) << 32) | t.height;
        auto [entry, inserted] = caps.rates.try_emplace(key);
        if (inserted) caps.sizes.push_back(SIZE{static_cast<LONG>(t.width), static_cast<LONG>(t.height)});
        const double rate = RoundRate(t.rate);
        if (rate > 0 && std::find(entry->second.begin(), entry->second.end(), rate) == entry->second.end()) {
            entry->second.push_back(rate);
        }
    }
    std::sort(caps.sizes.begin(), caps.sizes.end(), [](const SIZE& a, const SIZE& b) {
        const int64_t areaA = int64_t(a.cx) * a.cy, areaB = int64_t(b.cx) * b.cy;
        return areaA != areaB ? areaA > areaB : a.cx > b.cx;
    });
    for (auto& entry : caps.rates) std::sort(entry.second.begin(), entry.second.end(), std::greater<double>());
    return caps;
}

/// State shared by the VideoCapture object and every reader callback (callbacks may outlive the
/// reader they belong to, so this is reference counted).
struct Shared {
    HWND notify = nullptr;
    FrameSink sink;
    std::atomic<uint64_t> frames{0};

    // Guards liveSession, requestSerial, every callback's reader pointer and mode, and every sink
    // call, so "delivery stopped" is exact. Held only briefly (a sink call is cheap). Recursive
    // because OnReadSample calls ReadSample while holding it and nothing documents that the reader
    // never calls back on the same thread. Never held while the worker calls into a reader.
    std::recursive_mutex delivery;
    uint64_t liveSession = 0;    // the session allowed to deliver frames and report errors; 0 = none
    uint64_t requestSerial = 0;  // bumped by Start / Stop / destruction: an open in flight must not go live

    mutable std::mutex modeMutex;
    VideoMode mode;  // what Mode() returns
    VideoCaps caps;  // what Caps() returns: formats of the device last opened

    void SetMode(const VideoMode& m) {
        std::lock_guard<std::mutex> lock(modeMutex);
        mode = m;
    }
};

class ReaderCallback final : public IMFSourceReaderCallback {
public:
    ReaderCallback(std::shared_ptr<Shared> shared, uint64_t id, UINT generation)
        : shared_(std::move(shared)), id_(id), generation_(generation) {}

    HRESULT Init() {
        flushed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        first_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return flushed_ && first_ ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    uint64_t Id() const { return id_; }
    HANDLE FlushedEvent() const { return flushed_; }
    HANDLE FirstResultEvent() const { return first_; }
    bool SawFrame() const { return sawFrame_.load(); }

    // Caller holds shared->delivery.
    void Attach(IMFSourceReader* reader, const VideoMode& mode) {
        reader_ = reader;
        mode_ = mode;
        readTick_ = GetTickCount64();
    }
    void Detach() { reader_ = nullptr; }

    /// Worker, after the first-result wait: from now on failures go to WM_APP_VIDEO_ERROR, unless a
    /// read already failed, in which case that failure is the result of the start.
    HRESULT FinishStarting() {
        int expected = kStarting;
        if (phase_.compare_exchange_strong(expected, kRunning)) return S_OK;
        return firstFailure_.load();
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFSourceReaderCallback)) {
            *object = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return refs_.fetch_add(1) + 1; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = refs_.fetch_sub(1) - 1;
        if (left == 0) delete this;
        return left;
    }

    // IMFSourceReaderCallback (MF work-queue threads)
    STDMETHODIMP OnReadSample(HRESULT status, DWORD /*streamIndex*/, DWORD flags, LONGLONG /*timestamp*/,
                              IMFSample* sample) override {
        std::lock_guard<std::recursive_mutex> lock(shared_->delivery);
        // Closed, superseded or already failed: drop everything and do not read again.
        if (shared_->liveSession != id_ || !reader_ || failed_) return S_OK;

        if (FAILED(status) || (flags & MF_SOURCE_READERF_ERROR)) {
            Fail(FAILED(status) ? status : E_FAIL, L"Video: ReadSample completed with an error");
            return S_OK;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            Fail(MF_E_END_OF_STREAM, L"Video: ReadSample reported end of stream");
            return S_OK;
        }
        if (flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) {
            // A sample that comes with this flag is already in the new format.
            const HRESULT hr = RefreshMode(flags);
            if (FAILED(hr)) {
                Fail(hr, L"Video: re-reading the media type after a change");
                return S_OK;
            }
        }
        if (sample) {
            shared_->frames.fetch_add(1, std::memory_order_relaxed);
            if (!sawFrame_.exchange(true)) {
                Log(L"Video: session %llu first frame %llu ms after the first read", static_cast<unsigned long long>(id_),
                    static_cast<unsigned long long>(GetTickCount64() - readTick_));
                SetEvent(first_);
            }
            if (shared_->sink) {
                // No exception may cross back into Media Foundation.
                try {
                    shared_->sink(sample, mode_);
                } catch (...) {
                    if (!sinkThrew_) Log(L"Video: the frame sink threw an exception (logged once per session)");
                    sinkThrew_ = true;
                }
            }
        }
        // Ask for the next frame right away: one request in flight, nothing queued behind it.
        const HRESULT hr = reader_->ReadSample(kVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) Fail(hr, L"Video: ReadSample (next)");
        return S_OK;
    }

    STDMETHODIMP OnFlush(DWORD /*streamIndex*/) override {
        SetEvent(flushed_);
        return S_OK;
    }

    STDMETHODIMP OnEvent(DWORD streamIndex, IMFMediaEvent* event) override {
        // Log only, without the delivery lock (the reader may hold its own lock here). A removed
        // or pre-empted device also fails the pending read, and that reports the error; if it
        // does not, the App's watchdog restarts the stalled stream.
        if (!event || eventsLogged_.fetch_add(1) >= 32) return S_OK;
        MediaEventType type = MEUnknown;
        HRESULT status = S_OK;
        if (FAILED(event->GetType(&type))) type = MEUnknown;
        if (FAILED(event->GetStatus(&status))) status = S_OK;
        const wchar_t* name = type == MEVideoCaptureDeviceRemoved     ? L" (capture device removed)"
                              : type == MEVideoCaptureDevicePreempted ? L" (capture device pre-empted by another app)"
                              : type == MEError                       ? L" (error)"
                                                                      : L"";
        Log(L"Video: session %llu reader event %lu%ls on stream 0x%lX, status 0x%08lX",
            static_cast<unsigned long long>(id_), static_cast<unsigned long>(type), name,
            static_cast<unsigned long>(streamIndex), static_cast<unsigned long>(status));
        if (FAILED(status)) Check(status, L"Video: reader event status");
        return S_OK;
    }

private:
    enum : int { kStarting, kRunning, kFailed };

    ~ReaderCallback() {
        if (flushed_) CloseHandle(flushed_);
        if (first_) CloseHandle(first_);
    }

    // Caller holds shared->delivery and has checked that this session is live.
    void Fail(HRESULT hr, const wchar_t* what) {
        failed_ = true;  // no more reads for this session: the App stops or restarts it
        Check(hr, what);
        firstFailure_.store(hr);
        int expected = kStarting;
        if (phase_.compare_exchange_strong(expected, kFailed)) {
            SetEvent(first_);  // the worker reports it as the result of Start
            return;
        }
        Log(L"Video: session %llu failed while running (generation %u)", static_cast<unsigned long long>(id_),
            generation_);
        if (shared_->notify) {
            PostMessageW(shared_->notify, WM_APP_VIDEO_ERROR, static_cast<WPARAM>(generation_), static_cast<LPARAM>(hr));
        }
    }

    // Caller holds shared->delivery and has checked that this session is live.
    HRESULT RefreshMode(DWORD flags) {
        ComPtr<IMFMediaType> current;
        HRESULT hr = Check(reader_->GetCurrentMediaType(kVideoStream, &current), L"Video: GetCurrentMediaType");
        VideoMode mode;
        if (SUCCEEDED(hr)) hr = ReadMode(current.Get(), mode_.fps, mode);
        if (FAILED(hr)) return hr;
        Log(L"Video: session %llu media type changed (flags 0x%lX), was %ux%u @ %.3f",
            static_cast<unsigned long long>(id_), static_cast<unsigned long>(flags), mode_.width, mode_.height,
            mode_.fps);
        LogDeliveredType(current.Get(), mode, L"now delivering");
        mode_ = mode;
        shared_->SetMode(mode);
        // The window keeps the old aspect lock and menu labels until the App re-reads Mode().
        if (shared_->notify) PostMessageW(shared_->notify, WM_APP_VIDEO_MODE_CHANGED, static_cast<WPARAM>(generation_), 0);
        return S_OK;
    }

    std::atomic<ULONG> refs_{1};
    const std::shared_ptr<Shared> shared_;
    const uint64_t id_;
    const UINT generation_;
    HANDLE flushed_ = nullptr;
    HANDLE first_ = nullptr;
    std::atomic<int> phase_{kStarting};
    std::atomic<HRESULT> firstFailure_{S_OK};
    std::atomic<bool> sawFrame_{false};
    std::atomic<int> eventsLogged_{0};

    // Guarded by shared->delivery.
    IMFSourceReader* reader_ = nullptr;  // not owned: the reader owns us; Close() detaches before releasing it
    VideoMode mode_;
    bool failed_ = false;
    bool sinkThrew_ = false;
    ULONGLONG readTick_ = 0;
};

enum class Op { None, Start, Stop, Restart, Quit };

struct Request {
    Op op = Op::None;
    std::wstring link;
    std::optional<SIZE> size;
    std::optional<double> fps;
    UINT generation = 0;
    uint64_t serial = 0;  // Shared::requestSerial when queued; a newer Start / Stop changes it
};

struct Session {
    uint64_t id = 0;
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    ComPtr<ReaderCallback> callback;
};

}  // namespace

struct VideoCapture::Impl {
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    HANDLE wake = nullptr;  // auto-reset: a request was queued
    HANDLE finished = nullptr;  // manual reset: Run() returned
    std::thread worker;
    std::atomic<UINT> lastGeneration{0};

    std::mutex requestMutex;
    Request pending;       // guarded by requestMutex; a newer request replaces an older one
    bool opening = false;  // guarded by requestMutex: the worker is running a Start or Restart

    // Worker thread only.
    Session session;
    std::optional<Request> current;  // the last Start, re-opened by Restart
    uint64_t lastSessionId = 0;
    std::vector<std::wstring> typesLogged;  // links whose full type list is already in the log

    /// Caller thread: cuts frame delivery now and invalidates any open in flight.
    uint64_t Supersede() {
        std::lock_guard<std::recursive_mutex> lock(shared->delivery);
        shared->liveSession = 0;
        return ++shared->requestSerial;
    }

    uint64_t CurrentSerial() {
        std::lock_guard<std::recursive_mutex> lock(shared->delivery);
        return shared->requestSerial;
    }

    void Queue(Request request) {
        {
            std::lock_guard<std::mutex> lock(requestMutex);
            pending = std::move(request);
        }
        SetEvent(wake);
    }

    bool Running() const { return wake && worker.joinable(); }

    void Run();
    void Execute(const Request& request);
    void Open(const Request& target, uint64_t serial, bool restart);
    HRESULT OpenSession(const Request& target, uint64_t serial, bool& superseded);
    void WaitForFirstResult();
    void Close();
};

void VideoCapture::Impl::Run() {
    // MTA: reader callbacks arrive on MF work-queue threads anyway and nothing here needs a message
    // pump. MFStartup is reference counted; our own reference keeps MF alive for the teardown below
    // even if the App shuts it down first.
    const HRESULT com = LogIfFailed(CoInitializeEx(nullptr, COINIT_MULTITHREADED), L"Video: CoInitializeEx (worker)");
    const HRESULT mf = LogIfFailed(MFStartup(MF_VERSION, MFSTARTUP_LITE), L"Video: MFStartup (worker)");
    Log(L"Video: worker started");
    for (;;) {
        Request request;
        {
            std::lock_guard<std::mutex> lock(requestMutex);
            request = std::move(pending);
            pending = Request{};
            opening = request.op == Op::Start || request.op == Op::Restart;
        }
        if (request.op == Op::None) {
            WaitForSingleObject(wake, INFINITE);
            continue;
        }
        if (request.op == Op::Quit) break;
        Execute(request);
        std::lock_guard<std::mutex> lock(requestMutex);
        opening = false;
    }
    Close();
    if (SUCCEEDED(mf)) MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    Log(L"Video: worker stopped");
    if (finished) SetEvent(finished);
}

void VideoCapture::Impl::Execute(const Request& request) {
    switch (request.op) {
    case Op::Start:
        current = request;
        Open(request, request.serial, false);
        break;
    case Op::Restart:
        if (!current) {
            Log(L"Video: restart ignored, no device was started");
            break;
        }
        Open(*current, request.serial, true);
        break;
    case Op::Stop:
        current.reset();
        Close();
        shared->SetMode(VideoMode{});
        Log(L"Video: stopped");
        break;
    default:
        break;
    }
}

void VideoCapture::Impl::Open(const Request& target, uint64_t serial, bool restart) {
    const ULONGLONG started = GetTickCount64();
    Log(L"Video: %ls generation %u: %ls (%ls)", restart ? L"restarting" : L"starting", target.generation,
        target.link.c_str(), DescribeRequest(target.size, target.fps).c_str());
    Close();  // release the device before opening it again (some cards take only one client)

    bool superseded = false;
    HRESULT hr = OpenSession(target, serial, superseded);
    if (SUCCEEDED(hr) && !superseded) {
        WaitForFirstResult();
        hr = session.callback->FinishStarting();
        if (SUCCEEDED(hr) && !session.callback->SawFrame()) {
            Log(L"Video: no frame within %lu ms (no HDMI signal yet?); treating the stream as started",
                static_cast<unsigned long>(kFirstResultTimeoutMs));
        }
    }
    if (FAILED(hr) && !superseded) {
        Close();  // release the device before the App reacts (it may retry at once)
        if (!restart) shared->SetMode(VideoMode{});
    }
    {
        // Checked and posted under the delivery lock: a Stop that returned before this point
        // always wins, so the App never hears about a start it already cancelled.
        std::lock_guard<std::recursive_mutex> lock(shared->delivery);
        superseded = superseded || shared->requestSerial != serial;
        if (!superseded && shared->notify) {
            // A restart reports through its own message: a failed one must not re-enter the
            // 1 s error retry, but the App still needs the reason (privacy, device in use) and
            // the mode it landed on.
            PostMessageW(shared->notify, restart ? WM_APP_VIDEO_RESTARTED : WM_APP_VIDEO_STARTED,
                         static_cast<WPARAM>(target.generation), static_cast<LPARAM>(hr));
        }
    }
    const auto elapsed = static_cast<unsigned long long>(GetTickCount64() - started);
    if (superseded) {
        // A newer Start / Stop is queued; a superseded Start posts nothing (the App only cares
        // about its newest generation).
        Log(L"Video: generation %u superseded after %llu ms", target.generation, elapsed);
        Close();
    } else if (FAILED(hr)) {
        Log(L"Video: %ls generation %u failed after %llu ms (0x%08lX)%ls", restart ? L"restart of" : L"start of",
            target.generation, elapsed, static_cast<unsigned long>(hr),
            restart ? L"; the watchdog will try again" : L"");
    } else {
        Log(L"Video: generation %u %ls in %llu ms", target.generation, restart ? L"restarted" : L"started", elapsed);
    }
}

HRESULT VideoCapture::Impl::OpenSession(const Request& target, uint64_t serial, bool& superseded) {
    superseded = false;
    if (target.link.empty()) return E_INVALIDARG;
    session.id = ++lastSessionId;

    HRESULT hr = Check(CreateDeviceSource(target.link, session.source), L"Video: MFCreateDeviceSource");
    if (FAILED(hr)) return hr;

    session.callback.Attach(new (std::nothrow) ReaderCallback(shared, session.id, target.generation));
    if (!session.callback) return E_OUTOFMEMORY;
    hr = Check(session.callback->Init(), L"Video: CreateEvent for the reader callback");
    if (FAILED(hr)) return hr;

    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 3);
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, session.callback.Get());
    // No extra buffering in the reader or in a decoder.
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    // Lets the reader load the MJPEG decoder when the device only offers MJPG at the wanted size.
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (FAILED(Check(hr, L"Video: source reader attributes"))) return hr;

    hr = Check(MFCreateSourceReaderFromMediaSource(session.source.Get(), attributes.Get(), &session.reader),
               L"Video: MFCreateSourceReaderFromMediaSource");
    if (FAILED(hr)) return hr;

    // Capture sources can expose more streams (e.g. still images); only the first video stream is read.
    Check(session.reader->SetStreamSelection(kAllStreams, FALSE), L"Video: SetStreamSelection (all off)");
    hr = Check(session.reader->SetStreamSelection(kVideoStream, TRUE), L"Video: SetStreamSelection (video on)");
    if (FAILED(hr)) return hr;

    std::vector<NativeType> types;
    hr = Check(EnumerateReaderTypes(session.reader.Get(), types), L"Video: enumerating native types");
    if (FAILED(hr)) return hr;
    {
        VideoCaps caps = CapsFromTypes(types);
        std::lock_guard<std::mutex> lock(shared->modeMutex);
        shared->caps = std::move(caps);
    }
    const bool fullList = std::find(typesLogged.begin(), typesLogged.end(), target.link) == typesLogged.end();
    Log(L"Video: %llu native types%ls", static_cast<unsigned long long>(types.size()),
        fullList ? L":" : L" (listed earlier in this log)");
    if (fullList) {
        typesLogged.push_back(target.link);
        for (const NativeType& t : types) {
            Log(L"Video:   #%lu %ls%ls", static_cast<unsigned long>(t.index), TypeLabel(t).c_str(),
                t.Usable() ? L"" : L" (not usable)");
        }
    }

    const std::optional<size_t> choice = ChooseFormat(types, target.size, target.fps);
    if (!choice) {
        Log(L"Video: no usable native type (needs NV12, YUY2 or MJPG)");
        return MF_E_INVALIDMEDIATYPE;
    }
    const NativeType& native = types[*choice];
    Log(L"Video: chose #%lu %ls", static_cast<unsigned long>(native.index), TypeLabel(native).c_str());

    hr = ConfigureOutput(session.reader.Get(), native);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaType> currentType;
    hr = Check(session.reader->GetCurrentMediaType(kVideoStream, &currentType), L"Video: GetCurrentMediaType");
    VideoMode mode;
    if (SUCCEEDED(hr)) hr = Check(ReadMode(currentType.Get(), native.rate, mode), L"Video: delivered media type");
    if (FAILED(hr)) return hr;
    LogDeliveredType(currentType.Get(), mode, L"delivering");

    {
        // Go live, unless a newer Start / Stop arrived while the device was opening.
        std::lock_guard<std::recursive_mutex> lock(shared->delivery);
        if (shared->requestSerial != serial) {
            superseded = true;
            return S_OK;
        }
        session.callback->Attach(session.reader.Get(), mode);
        shared->liveSession = session.id;
        shared->SetMode(mode);
    }
    // Outside the lock: the worker never calls into a reader while holding it.
    return Check(session.reader->ReadSample(kVideoStream, 0, nullptr, nullptr, nullptr, nullptr),
                 L"Video: first ReadSample");
}

void VideoCapture::Impl::WaitForFirstResult() {
    const ULONGLONG deadline = GetTickCount64() + kFirstResultTimeoutMs;
    const HANDLE handles[] = {session.callback->FirstResultEvent(), wake};
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return;
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, static_cast<DWORD>(deadline - now));
        if (wait != WAIT_OBJECT_0 + 1) return;  // first frame / failure, timeout, or a wait error
        // Woken by a request: stop waiting if one is really pending (Run picks it up next without
        // waiting for `wake` again); otherwise it was a stale wake-up.
        std::lock_guard<std::mutex> lock(requestMutex);
        if (pending.op != Op::None) return;
    }
}

void VideoCapture::Impl::Close() {
    if (!session.source && !session.reader) {
        session = Session{};
        return;
    }
    const ULONGLONG started = GetTickCount64();
    const uint64_t id = session.id;
    {
        std::lock_guard<std::recursive_mutex> lock(shared->delivery);
        if (shared->liveSession == id) shared->liveSession = 0;
    }
    if (session.reader) {
        // Cancel the outstanding read and wait until the reader is idle, so no callback is still
        // working inside the pipeline while it is released.
        if (session.callback) ResetEvent(session.callback->FlushedEvent());
        const HRESULT hr = session.reader->Flush(kAllStreams);
        if (SUCCEEDED(hr) && session.callback) {
            if (WaitForSingleObject(session.callback->FlushedEvent(), kFlushTimeoutMs) != WAIT_OBJECT_0) {
                Log(L"Video: session %llu flush did not finish within %lu ms; releasing anyway",
                    static_cast<unsigned long long>(id), static_cast<unsigned long>(kFlushTimeoutMs));
            }
        } else if (FAILED(hr) && hr != MF_E_SHUTDOWN) {
            Check(hr, L"Video: IMFSourceReader::Flush");
        }
    }
    if (session.callback) {
        std::lock_guard<std::recursive_mutex> lock(shared->delivery);
        session.callback->Detach();
    }
    session.reader.Reset();
    if (session.source) {
        // The reader shuts the source down only when its last reference goes, which MF may hold on
        // to for a while; shutting it down here frees the device so it can be reopened at once.
        const HRESULT hr = session.source->Shutdown();
        if (FAILED(hr) && hr != MF_E_SHUTDOWN) Check(hr, L"Video: IMFMediaSource::Shutdown");
    }
    session = Session{};
    Log(L"Video: session %llu closed in %llu ms", static_cast<unsigned long long>(id),
        static_cast<unsigned long long>(GetTickCount64() - started));
}

VideoCapture::VideoCapture(HWND notify, FrameSink sink) : impl_(std::make_unique<Impl>()) {
    impl_->shared->notify = notify;
    impl_->shared->sink = std::move(sink);
    impl_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl_->finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->wake) {
        LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Video: CreateEvent (worker wake-up)");
        return;
    }
    try {
        Impl* impl = impl_.get();
        impl_->worker = std::thread([impl] { impl->Run(); });
    } catch (...) {
        Log(L"Video: could not create the worker thread; video is unavailable");
    }
}

VideoCapture::~VideoCapture() {
    // From here on the sink is never called again (it may point into objects being destroyed).
    impl_->Supersede();
    if (impl_->Running()) {
        Request quit;
        quit.op = Op::Quit;
        impl_->Queue(std::move(quit));
        // std::thread has no timed join. A driver call that never returns must not keep a
        // windowless process alive: give up and leave the worker the state it still uses.
        if (impl_->finished && WaitForSingleObject(impl_->finished, kWorkerExitTimeoutMs) == WAIT_TIMEOUT) {
            Log(L"Video: the worker did not stop within %lu ms (stuck in the capture driver?); leaving it behind",
                static_cast<unsigned long>(kWorkerExitTimeoutMs));
            impl_->worker.detach();
            NoteAbandonedThread();
            (void)impl_.release();  // deliberately leaked
            return;
        }
        impl_->worker.join();
    }
    if (impl_->wake) CloseHandle(impl_->wake);
    if (impl_->finished) CloseHandle(impl_->finished);
}

UINT VideoCapture::Start(const std::wstring& symbolicLink, std::optional<SIZE> size, std::optional<double> fps) {
    const UINT generation = impl_->lastGeneration.fetch_add(1) + 1;
    Log(L"Video: Start requested, generation %u: %ls (%ls)", generation, symbolicLink.c_str(),
        DescribeRequest(size, fps).c_str());
    Request request;
    request.op = Op::Start;
    request.link = symbolicLink;
    request.size = size;
    request.fps = fps;
    request.generation = generation;
    request.serial = impl_->Supersede();  // the previous stream stops delivering now
    if (impl_->Running()) {
        impl_->Queue(std::move(request));
    } else if (impl_->shared->notify) {
        PostMessageW(impl_->shared->notify, WM_APP_VIDEO_STARTED, static_cast<WPARAM>(generation),
                     static_cast<LPARAM>(E_UNEXPECTED));
    }
    return generation;
}

void VideoCapture::Stop() {
    Log(L"Video: Stop requested");
    Request request;
    request.op = Op::Stop;
    request.serial = impl_->Supersede();
    if (impl_->Running()) impl_->Queue(std::move(request));
}

void VideoCapture::Restart() {
    if (!impl_->Running()) return;
    Request request;
    request.op = Op::Restart;
    // Restart does not supersede: it only re-opens what is there, and a Start in progress already
    // opens the device fresh.
    request.serial = impl_->CurrentSerial();
    {
        std::lock_guard<std::mutex> lock(impl_->requestMutex);
        if (impl_->pending.op != Op::None || impl_->opening) {
            Log(L"Video: Restart ignored, a start or restart is already in progress");
            return;
        }
        impl_->pending = std::move(request);
    }
    Log(L"Video: Restart requested");
    SetEvent(impl_->wake);
}

VideoMode VideoCapture::Mode() const {
    std::lock_guard<std::mutex> lock(impl_->shared->modeMutex);
    return impl_->shared->mode;
}

uint64_t VideoCapture::FramesReceived() const { return impl_->shared->frames.load(std::memory_order_relaxed); }

VideoCaps VideoCapture::Caps() const {
    std::lock_guard<std::mutex> lock(impl_->shared->modeMutex);
    return impl_->shared->caps;
}

VideoCaps VideoCapture::Capabilities(const std::wstring& symbolicLink) {
    VideoCaps caps;
    if (symbolicLink.empty()) return caps;
    const ULONGLONG started = GetTickCount64();
    // Works on any thread: joins the caller's apartment (S_FALSE / RPC_E_CHANGED_MODE on the UI
    // thread), or makes a temporary MTA one.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    size_t typeCount = 0;
    {
        std::vector<NativeType> types;
        ComPtr<IMFMediaSource> source;
        HRESULT hr = Check(CreateDeviceSource(symbolicLink, source), L"Video: MFCreateDeviceSource (capabilities)");
        if (SUCCEEDED(hr)) {
            Check(EnumerateSourceTypes(source.Get(), types), L"Video: enumerating native types (capabilities)");
            // Nothing was started; release the device right away.
            hr = source->Shutdown();
            if (FAILED(hr) && hr != MF_E_SHUTDOWN) Check(hr, L"Video: IMFMediaSource::Shutdown (capabilities)");
        }
        typeCount = types.size();
        caps = CapsFromTypes(types);
    }  // media types and the source are released before leaving a temporary apartment
    if (SUCCEEDED(com)) CoUninitialize();

    Log(L"Video: capabilities of %ls: %llu sizes from %llu native types in %llu ms", symbolicLink.c_str(),
        static_cast<unsigned long long>(caps.sizes.size()), static_cast<unsigned long long>(typeCount),
        static_cast<unsigned long long>(GetTickCount64() - started));
    for (const SIZE& size : caps.sizes) {
        std::wstring rates;
        for (double rate : caps.RatesFor(static_cast<UINT32>(size.cx), static_cast<UINT32>(size.cy))) {
            wchar_t text[32];
            swprintf(text, 32, rates.empty() ? L"%.2f" : L", %.2f", rate);
            rates += text;
        }
        Log(L"Video:   %ldx%ld: %ls", static_cast<long>(size.cx), static_cast<long>(size.cy), rates.c_str());
    }
    return caps;
}
