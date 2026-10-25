// Direct3D 11 presentation of the newest captured frame.
//
// SubmitFrame (a Media Foundation worker thread) copies the locked buffer into CPU memory and
// wakes the render thread; it never touches the GPU, so capture timing does not depend on
// presentation. The render thread is the only user of the immediate context: it uploads the
// newest frame, decodes YUV to RGBA once per frame (pass 1, source size, exact texel loads),
// draws that letterboxed with the picture adjustments (pass 2, bilinear), adds Direct2D text
// overlays and presents with Present(0, 0) on a flip-model swap chain that queues at most one
// frame. Nothing is presented unless something on screen changed.

#include "Renderer.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <mfapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>
#include <utility>

namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

constexpr UINT kSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
constexpr Milliseconds kIdleWait{100};      // wake up periodically even when nothing happens
constexpr Milliseconds kLatencyWait{100};   // longest wait for the swap chain to take a frame
constexpr Milliseconds kSnapshotWait{100};  // RequestSnapshot waits this long for a new frame
constexpr Milliseconds kToastHold{1200};
constexpr Milliseconds kToastFade{300};
constexpr Milliseconds kFadeStep{16};
constexpr Milliseconds kDeviceRetry{500};
constexpr std::chrono::seconds kStatsInterval{60};
constexpr UINT kMaxFrameSide = 16384;  // D3D11 texture limit at feature level 11

enum class PixelFormat { None, NV12, YUY2 };

const wchar_t* FormatName(PixelFormat format) {
    switch (format) {
    case PixelFormat::NV12: return L"NV12";
    case PixelFormat::YUY2: return L"YUY2";
    default: return L"none";
    }
}

UINT ChromaWidth(UINT width) { return (width + 1) / 2; }
UINT ChromaHeight(UINT height) { return (height + 1) / 2; }

/// Frame bytes in CpuFrame::data, rows packed without padding:
/// NV12 = Y plane (width per row) then interleaved CbCr (half height, ChromaWidth * 2 per row);
/// YUY2 = one Y0 Cb Y1 Cr quad per pixel pair.
size_t FrameBytes(PixelFormat format, UINT width, UINT height) {
    switch (format) {
    case PixelFormat::NV12:
        return size_t(width) * height + size_t(ChromaWidth(width)) * 2 * ChromaHeight(height);
    case PixelFormat::YUY2:
        return size_t(ChromaWidth(width)) * 4 * height;
    default:
        return 0;
    }
}

struct CpuFrame {
    std::vector<uint8_t> data;
    UINT width = 0;
    UINT height = 0;
    PixelFormat format = PixelFormat::None;
};

/// How frames are arriving; logged whenever it changes so the log shows the buffer layout.
struct SubmitInfo {
    UINT width = 0;
    UINT height = 0;
    PixelFormat format = PixelFormat::None;
    LONG pitch = 0;
    const wchar_t* method = L"";
    bool operator==(const SubmitInfo&) const = default;
};

/// Per-frame failures: log the first few, then every 600th (about every 10 s at 60 fps), so a
/// persistent problem is visible without flooding the log.
bool ShouldLogRepeated(std::atomic<uint32_t>& counter) {
    const uint32_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    return n <= 5 || n % 600 == 0;
}

bool IsDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR || hr == D2DERR_RECREATE_TARGET;
}

unsigned Hex(HRESULT hr) { return static_cast<unsigned>(hr); }

/// Locks an IMFMediaBuffer for reading. Prefers the 2-D interfaces because only they report the
/// real pitch (capture drivers often pad rows); Lock2DSize also reports the bounds to check.
class LockedBuffer {
public:
    LockedBuffer() = default;
    LockedBuffer(const LockedBuffer&) = delete;
    LockedBuffer& operator=(const LockedBuffer&) = delete;
    ~LockedBuffer() {
        if (buffer2d_) {
            buffer2d_->Unlock2D();
        } else if (buffer_) {
            buffer_->Unlock();
        }
    }

    HRESULT Lock(IMFMediaBuffer* buffer) {
        ComPtr<IMF2DBuffer2> buffer2;
        if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2)))) {
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            BYTE* start = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(buffer2->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &length))) {
                buffer2d_ = buffer2.Get();
                scan0_ = scan0;
                pitch_ = pitch;
                start_ = start;
                length_ = length;
                method_ = L"Lock2DSize";
                return S_OK;
            }
        }
        ComPtr<IMF2DBuffer> buffer1;
        if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer1)))) {
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(buffer1->Lock2D(&scan0, &pitch))) {
                buffer2d_ = buffer1;
                scan0_ = scan0;
                pitch_ = pitch;
                method_ = L"Lock2D";
                // No bounds from Lock2D; the packed size at least catches a frame smaller than
                // the size we were told (e.g. while the format changes).
                DWORD contiguous = 0;
                if (SUCCEEDED(buffer1->GetContiguousLength(&contiguous))) packedLength_ = contiguous;
                return S_OK;
            }
        }
        BYTE* data = nullptr;
        DWORD maxLength = 0;
        DWORD length = 0;
        const HRESULT hr = buffer->Lock(&data, &maxLength, &length);
        if (FAILED(hr)) return hr;
        buffer_ = buffer;
        scan0_ = data;
        pitch_ = 0;  // unknown: the caller derives it
        start_ = data;
        length_ = length;
        method_ = L"Lock";
        return S_OK;
    }

    const uint8_t* Scan0() const { return scan0_; }
    LONG Pitch() const { return pitch_; }
    const uint8_t* Start() const { return start_; }  // nullptr when the bounds are unknown
    size_t Length() const { return length_; }
    size_t PackedLength() const { return packedLength_; }  // Lock2D only: the image size without padding
    const wchar_t* Method() const { return method_; }

private:
    ComPtr<IMF2DBuffer> buffer2d_;
    ComPtr<IMFMediaBuffer> buffer_;
    const uint8_t* scan0_ = nullptr;
    LONG pitch_ = 0;
    const uint8_t* start_ = nullptr;
    size_t length_ = 0;
    size_t packedLength_ = 0;
    const wchar_t* method_ = L"";
};

/// Copies the picture out of a locked buffer into packed rows (see FrameBytes).
/// Returns nullptr on success, otherwise why the frame was dropped.
const wchar_t* CopyFrame(const LockedBuffer& locked, PixelFormat format, UINT width, UINT height,
                         std::vector<uint8_t>& out, SubmitInfo& info) {
    const bool nv12 = format == PixelFormat::NV12;
    const size_t lumaRow = nv12 ? size_t(width) : size_t(ChromaWidth(width)) * 4;
    const size_t chromaRow = nv12 ? size_t(ChromaWidth(width)) * 2 : 0;
    const size_t chromaRows = nv12 ? ChromaHeight(height) : 0;
    const size_t widestRow = std::max(lumaRow, chromaRow);
    const size_t memoryRows = size_t(height) + chromaRows;

    LONG pitch = locked.Pitch();
    if (pitch == 0) {
        // A plain IMFMediaBuffer carries no pitch: assume packed rows unless the length is an
        // exact multiple of a slightly wider, aligned row (padded driver buffers).
        pitch = LONG(widestRow);
        const size_t length = locked.Length();
        if (length % memoryRows == 0) {
            const size_t candidate = length / memoryRows;
            if (candidate > widestRow && candidate % 16 == 0 && candidate - widestRow < 1024) {
                pitch = LONG(candidate);
            }
        }
    }
    info.pitch = pitch;
    info.method = locked.Method();
    if (pitch < 0 && nv12) return L"bottom-up NV12 buffer";
    const size_t absPitch = pitch < 0 ? size_t(-int64_t(pitch)) : size_t(pitch);
    if (absPitch < widestRow) return L"pitch smaller than a row";
    if (!locked.Scan0()) return L"null buffer";

    if (locked.Start()) {
        // Work on integers: out-of-range pointer arithmetic would itself be undefined.
        const int64_t begin = int64_t(reinterpret_cast<uintptr_t>(locked.Start()));
        const int64_t end = begin + int64_t(locked.Length());
        const int64_t first = int64_t(reinterpret_cast<uintptr_t>(locked.Scan0()));
        const int64_t lastRow = first + int64_t(pitch) * int64_t(memoryRows - 1);
        const int64_t low = std::min(first, lastRow);
        const int64_t high = std::max(first, lastRow) + int64_t(widestRow);
        if (low < begin || high > end) return L"buffer smaller than the frame";
    } else if (locked.PackedLength() != 0 && locked.PackedLength() < FrameBytes(format, width, height)) {
        return L"buffer smaller than the frame";
    }

    out.resize(FrameBytes(format, width, height));
    uint8_t* dst = out.data();
    auto copyRows = [&](const uint8_t* src, size_t rows, size_t rowBytes) {
        if (pitch > 0 && size_t(pitch) == rowBytes) {
            std::memcpy(dst, src, rows * rowBytes);
            dst += rows * rowBytes;
            return;
        }
        for (size_t y = 0; y < rows; ++y) {
            std::memcpy(dst, src + ptrdiff_t(y) * pitch, rowBytes);
            dst += rowBytes;
        }
    };
    copyRows(locked.Scan0(), height, lumaRow);
    if (nv12) {
        // The interleaved CbCr plane follows the Y plane with the same pitch.
        copyRows(locked.Scan0() + ptrdiff_t(pitch) * ptrdiff_t(height), chromaRows, chromaRow);
    }
    return nullptr;
}

D3D11_VIEWPORT FullViewport(UINT width, UINT height) {
    D3D11_VIEWPORT viewport{};
    viewport.Width = float(width);
    viewport.Height = float(height);
    viewport.MaxDepth = 1.0f;
    return viewport;
}

/// The largest rectangle with the source aspect ratio, centered, on whole pixels (black bars).
D3D11_VIEWPORT Letterbox(UINT targetWidth, UINT targetHeight, UINT sourceWidth, UINT sourceHeight) {
    targetWidth = std::max(targetWidth, 1u);
    targetHeight = std::max(targetHeight, 1u);
    const double scale = std::min(double(targetWidth) / sourceWidth, double(targetHeight) / sourceHeight);
    const double width = std::clamp(std::round(sourceWidth * scale), 1.0, double(targetWidth));
    const double height = std::clamp(std::round(sourceHeight * scale), 1.0, double(targetHeight));
    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = float(std::floor((targetWidth - width) / 2));
    viewport.TopLeftY = float(std::floor((targetHeight - height) / 2));
    viewport.Width = float(width);
    viewport.Height = float(height);
    viewport.MaxDepth = 1.0f;
    return viewport;
}

/// Pass 2 constants; layout matches the HLSL cbuffer (16-byte rows).
struct PictureConstants {
    float hueRow0[4];
    float hueRow1[4];
    float hueRow2[4];
    float brightness;
    float contrast;
    float saturation;
    float inverseGamma;
    float expandRange;
    float applyGamma;
    float applyHue;
    float padding;
};
static_assert(sizeof(PictureConstants) % 16 == 0, "constant buffers are sized in 16-byte rows");

PictureConstants MakePictureConstants(const PictureSettings& picture) {
    PictureConstants c{};
    // Core Image CIHueAdjust (luma-preserving hue rotation, Rec.709 weights), so a 색조 value
    // looks the same as on macOS: M = A + cos(angle) * B + sin(angle) * C.
    static constexpr double A[3] = {0.2125, 0.7154, 0.0721};
    static constexpr double B[3][3] = {
        {0.7875, -0.7154, -0.0721},
        {-0.2125, 0.2846, -0.0721},
        {-0.2125, -0.7154, 0.9279},
    };
    static constexpr double C[3][3] = {
        {-0.2125, -0.7154, 0.9279},
        {0.1404, 0.1404, -0.2846},
        {-0.7875, 0.7154, 0.0721},
    };
    const double angle = picture.hue * std::numbers::pi / 180.0;
    const double cosA = std::cos(angle);
    const double sinA = std::sin(angle);
    float rows[3][3];
    for (int r = 0; r < 3; ++r) {
        for (int col = 0; col < 3; ++col) {
            rows[r][col] = float(A[col] + cosA * B[r][col] + sinA * C[r][col]);
        }
    }
    std::memcpy(c.hueRow0, rows[0], sizeof(rows[0]));
    std::memcpy(c.hueRow1, rows[1], sizeof(rows[1]));
    std::memcpy(c.hueRow2, rows[2], sizeof(rows[2]));
    c.brightness = float(picture.brightness);
    c.contrast = float(picture.contrast);
    c.saturation = float(picture.saturation);
    c.inverseGamma = float(1.0 / std::max(picture.gamma, 0.01));
    c.expandRange = picture.expandRange ? 1.0f : 0.0f;
    c.applyGamma = picture.gamma != 1.0 ? 1.0f : 0.0f;
    c.applyHue = picture.hue != 0.0 ? 1.0f : 0.0f;
    return c;
}

// ---- HLSL (compiled at runtime with D3DCompile) ----

const char kShaderCommon[] = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// BT.709 limited range (Y 16-235, CbCr 16-240) to gamma-encoded R'G'B'.
float3 Bt709LimitedToRgb(float y, float cb, float cr) {
    y = (y - 16.0 / 255.0) * (255.0 / 219.0);
    cb = (cb - 128.0 / 255.0) * (255.0 / 224.0);
    cr = (cr - 128.0 / 255.0) * (255.0 / 224.0);
    return saturate(float3(y + 1.5748 * cr,
                           y - 0.187324 * cb - 0.468124 * cr,
                           y + 1.8556 * cb));
}
)";

// One triangle covering the viewport; uv runs 0..1 across the visible part.
const char kVertexShader[] = R"(
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
)";

// Pass 1 runs at source size, so SV_Position names the source pixel: exact loads, no filtering.
const char kDecodeNV12Shader[] = R"(
Texture2D<float> lumaPlane : register(t0);
Texture2D<float2> chromaPlane : register(t1);

float4 main(VSOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float y = lumaPlane.Load(int3(p, 0));
    float2 c = chromaPlane.Load(int3(p >> 1, 0));
    return float4(Bt709LimitedToRgb(y, c.x, c.y), 1.0);
}
)";

// YUY2 uploaded as RGBA8 at half width: r = Y0, g = Cb, b = Y1, a = Cr.
const char kDecodeYUY2Shader[] = R"(
Texture2D<float4> pixelPairs : register(t0);

float4 main(VSOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float4 t = pixelPairs.Load(int3(p.x >> 1, p.y, 0));
    float y = (p.x & 1) ? t.b : t.r;
    return float4(Bt709LimitedToRgb(y, t.g, t.a), 1.0);
}
)";

// Pass 2 with neutral settings: a plain scaled copy.
const char kCopyShader[] = R"(
Texture2D<float4> sourceImage : register(t0);
SamplerState bilinear : register(s0);

float4 main(VSOut i) : SV_Target {
    return float4(sourceImage.Sample(bilinear, i.uv).rgb, 1.0);
}
)";

// Pass 2 with picture adjustments, on gamma-encoded values, in the order of the macOS app.
const char kAdjustShader[] = R"(
Texture2D<float4> sourceImage : register(t0);
SamplerState bilinear : register(s0);

cbuffer PictureConstants : register(b0) {
    float4 hueRow0;
    float4 hueRow1;
    float4 hueRow2;
    float brightness;
    float contrast;
    float saturation;
    float inverseGamma;
    float expandRange;
    float applyGamma;
    float applyHue;
    float padding;
};

float4 main(VSOut i) : SV_Target {
    float3 c = sourceImage.Sample(bilinear, i.uv).rgb;
    if (expandRange > 0.5) {
        c = saturate((c - 16.0 / 255.0) * (255.0 / 219.0));
    }
    if (applyGamma > 0.5) {
        c = pow(max(c, 0.0), inverseGamma);
    }
    c = (c - 0.5) * contrast + 0.5 + brightness;
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(float3(luma, luma, luma), c, saturation);
    if (applyHue > 0.5) {
        c = float3(dot(hueRow0.xyz, c), dot(hueRow1.xyz, c), dot(hueRow2.xyz, c));
    }
    return float4(saturate(c), 1.0);
}
)";

/// DXGI may send messages to the window's thread from the render thread (swap chain calls), so
/// keep dispatching sent messages while waiting rather than blocking the UI thread outright.
void WaitForThreadExit(HANDLE thread) {
    for (;;) {
        const DWORD result = MsgWaitForMultipleObjects(1, &thread, FALSE, INFINITE, QS_SENDMESSAGE);
        if (result == WAIT_OBJECT_0 + 1) {
            MSG msg;
            PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            continue;
        }
        if (result == WAIT_FAILED) {
            Log(L"Renderer: waiting for the render thread failed (%lu)", GetLastError());
            WaitForSingleObject(thread, INFINITE);
        }
        return;
    }
}

}  // namespace

struct Renderer::Impl {
    Impl();
    ~Impl();

    HRESULT Initialize(HWND hwnd);
    void Shutdown();
    void Submit(IMFSample* sample, UINT32 width, UINT32 height, const GUID& subtype);
    void Clear();
    void Resize(UINT width, UINT height);
    void SetPicture(const PictureSettings& picture);
    void SetMessage(const std::wstring& text);
    void ShowToast(const std::wstring& text);
    void SetVisible(bool visible);
    void RequestSnapshot(HWND notify);

private:
    static DWORD WINAPI ThreadMain(LPVOID param);
    void RenderLoop();
    DWORD NextTimeout(Clock::time_point now) const;

    // Device lifetime (UI thread in Initialize/Shutdown, render thread while it runs).
    HRESULT LoadCompiler();
    void CreateFactories();
    HRESULT CreateDeviceResources();
    HRESULT CreateDevice();
    HRESULT CreateSwapChain();
    HRESULT CreateBackBufferView();
    HRESULT CreatePipeline();
    HRESULT CompileShader(const char* body, const char* profile, const wchar_t* name, ComPtr<ID3DBlob>& blob);
    HRESULT CreateOverlayResources();
    void ReleaseFrameTextures();
    void ReleaseDeviceResources();
    void OnDeviceLost(HRESULT hr, const wchar_t* where);
    bool TryRecoverDevice(Clock::time_point now);

    // Render thread.
    void ResizeSwapChain(UINT width, UINT height);
    void UpdatePictureConstants();
    void WaitForSwapChain();
    bool TakePendingFrame();
    bool EnsureFrameTextures(UINT width, UINT height, PixelFormat format);
    bool UploadAndDecode();
    void DrawPicture(const D3D11_VIEWPORT& viewport);
    void DrawScene();
    void EnsureTextFormats();
    void DrawOverlays(Clock::time_point now);
    void PresentScene(Clock::time_point now);
    void ServeSnapshots(bool freshFrame);
    std::unique_ptr<SnapshotImage> RenderSnapshot();
    void LogStats();

    HWND hwnd_ = nullptr;
    HANDLE thread_ = nullptr;
    HANDLE wakeEvent_ = nullptr;  // auto-reset: any change below, or quit
    std::atomic<bool> accepting_{false};

    // ---- Guarded by mutex_ (written by any thread, consumed by the render thread) ----
    std::mutex mutex_;
    struct Shared {
        bool quit = false;
        // Triple buffer: the producer fills `spare` outside the lock and swaps it with `pending`;
        // the render thread swaps `pending` with its `current_`. Newest frame wins.
        CpuFrame pending;
        CpuFrame spare;
        bool hasPending = false;
        bool clear = false;
        bool resize = false;
        UINT width = 0;
        UINT height = 0;
        bool pictureChanged = true;
        PictureSettings picture;
        bool messageChanged = false;
        std::wstring message;
        bool toastChanged = false;
        std::wstring toast;
        Clock::time_point toastStart;
        bool visible = true;
        std::vector<HWND> snapshotTargets;
        Clock::time_point snapshotDeadline;
        uint64_t framesSubmitted = 0;
        uint64_t framesReplaced = 0;  // overwritten by a newer frame before being drawn
        SubmitInfo lastSubmit;
    } shared_;

    std::atomic<uint32_t> rejectedFrames_{0};
    std::atomic<uint32_t> bufferFailures_{0};
    std::atomic<uint32_t> copyFailures_{0};

    // ---- Device-independent ----
    HMODULE compilerModule_ = nullptr;
    pD3DCompile compile_ = nullptr;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<IDWriteFactory> dwrite_;

    // ---- Device resources (render thread after Initialize) ----
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    D3D_FEATURE_LEVEL featureLevel_ = D3D_FEATURE_LEVEL_10_0;
    ComPtr<IDXGISwapChain2> swapChain_;
    HANDLE latencyWaitable_ = nullptr;
    bool haveLatencyToken_ = false;  // waited for the swap chain but not presented yet
    ComPtr<ID3D11RenderTargetView> backBufferView_;
    UINT backBufferWidth_ = 0;
    UINT backBufferHeight_ = 0;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> decodeNV12_;
    ComPtr<ID3D11PixelShader> decodeYUY2_;
    ComPtr<ID3D11PixelShader> copyShader_;
    ComPtr<ID3D11PixelShader> adjustShader_;
    ComPtr<ID3D11SamplerState> bilinear_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11Buffer> pictureBuffer_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2dContext_;
    ComPtr<ID2D1SolidColorBrush> brush_;

    // Frame textures: NV12 as R8 (Y) + R8G8 (CbCr), YUY2 as RGBA8 at half width; decoded RGBA.
    ComPtr<ID3D11Texture2D> lumaTexture_;
    ComPtr<ID3D11ShaderResourceView> lumaView_;
    ComPtr<ID3D11Texture2D> chromaTexture_;
    ComPtr<ID3D11ShaderResourceView> chromaView_;
    ComPtr<ID3D11Texture2D> packedTexture_;
    ComPtr<ID3D11ShaderResourceView> packedView_;
    ComPtr<ID3D11Texture2D> rgbTexture_;
    ComPtr<ID3D11RenderTargetView> rgbTarget_;
    ComPtr<ID3D11ShaderResourceView> rgbView_;
    UINT textureWidth_ = 0;
    UINT textureHeight_ = 0;
    PixelFormat textureFormat_ = PixelFormat::None;

    // ---- Render thread state ----
    CpuFrame current_;        // last frame taken; kept to re-upload after a device loss
    bool frameValid_ = false;  // rgbTexture_ holds a decoded picture
    bool deviceLost_ = false;
    Clock::time_point nextRecovery_;
    PictureSettings picture_;
    bool pictureNeutral_ = true;
    std::wstring message_;
    std::wstring toast_;
    Clock::time_point toastStart_;
    bool toastActive_ = false;
    bool snapshotWaiting_ = false;
    Clock::time_point snapshotDeadline_;
    UINT formatDpi_ = 0;
    ComPtr<IDWriteTextFormat> messageFormat_;
    ComPtr<IDWriteTextFormat> toastFormat_;
    ComPtr<IDWriteTextLayout> messageLayout_;
    std::wstring messageLayoutText_;
    float messageLayoutWidth_ = 0;
    ComPtr<IDWriteTextLayout> toastLayout_;
    std::wstring toastLayoutText_;
    uint64_t framesDecoded_ = 0;
    uint64_t framesPresented_ = 0;
    uint64_t statsSubmitted_ = 0;
    Clock::time_point lastStats_;
    std::atomic<uint32_t> presentFailures_{0};
    std::atomic<uint32_t> latencyTimeouts_{0};
    std::atomic<uint32_t> textureFailures_{0};
    std::atomic<uint32_t> overlayFailures_{0};
};

// ============================================================================================
// Public API (any thread unless noted)
// ============================================================================================

Renderer::Impl::Impl() {
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wakeEvent_) Log(L"Renderer: CreateEvent failed (%lu)", GetLastError());
}

Renderer::Impl::~Impl() {
    Shutdown();
    if (wakeEvent_) CloseHandle(wakeEvent_);
    d2dFactory_.Reset();
    dwrite_.Reset();
    if (compilerModule_) FreeLibrary(compilerModule_);
}

HRESULT Renderer::Impl::Initialize(HWND hwnd) {
    if (thread_) {
        Log(L"Renderer: Initialize called while running; restarting");
        Shutdown();
    }
    if (!hwnd || !IsWindow(hwnd)) return LogIfFailed(E_INVALIDARG, L"Renderer: Initialize without a window");
    if (!wakeEvent_) return E_OUTOFMEMORY;
    hwnd_ = hwnd;

    HRESULT hr = LoadCompiler();
    if (FAILED(hr)) return hr;
    CreateFactories();
    hr = CreateDeviceResources();
    if (FAILED(hr)) return hr;

    {
        std::lock_guard lock(mutex_);
        shared_.quit = false;
        shared_.pictureChanged = true;  // upload the current settings on the first pass
    }
    deviceLost_ = false;
    lastStats_ = Clock::now();
    thread_ = CreateThread(nullptr, 0, &Impl::ThreadMain, this, 0, nullptr);
    if (!thread_) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        LogIfFailed(hr, L"Renderer: CreateThread");
        ReleaseDeviceResources();
        return hr;
    }
    accepting_.store(true, std::memory_order_release);
    Log(L"Renderer: started (%ux%u back buffer)", backBufferWidth_, backBufferHeight_);
    return S_OK;
}

void Renderer::Impl::Shutdown() {
    accepting_.store(false, std::memory_order_release);
    if (thread_) {
        {
            std::lock_guard lock(mutex_);
            shared_.quit = true;
        }
        SetEvent(wakeEvent_);
        WaitForThreadExit(thread_);
        CloseHandle(thread_);
        thread_ = nullptr;
        LogStats();
        Log(L"Renderer: stopped");
    }
    // Nobody will render these any more; answer them so the UI is not left waiting.
    std::vector<HWND> targets;
    {
        std::lock_guard lock(mutex_);
        targets.swap(shared_.snapshotTargets);
        shared_.hasPending = false;
        shared_.pending = CpuFrame{};
        shared_.spare = CpuFrame{};
    }
    for (HWND target : targets) PostMessageW(target, WM_APP_SNAPSHOT, 0, 0);
    ReleaseDeviceResources();
    current_ = CpuFrame{};
    messageFormat_.Reset();
    toastFormat_.Reset();
    messageLayout_.Reset();
    toastLayout_.Reset();
    formatDpi_ = 0;
}

void Renderer::Impl::Submit(IMFSample* sample, UINT32 width, UINT32 height, const GUID& subtype) {
    if (!sample || !accepting_.load(std::memory_order_acquire)) return;
    const PixelFormat format = subtype == MFVideoFormat_NV12   ? PixelFormat::NV12
                               : subtype == MFVideoFormat_YUY2 ? PixelFormat::YUY2
                                                               : PixelFormat::None;
    if (format == PixelFormat::None || width == 0 || height == 0 || width > kMaxFrameSide || height > kMaxFrameSide) {
        if (ShouldLogRepeated(rejectedFrames_)) {
            Log(L"Renderer: ignoring %ux%u frame with subtype %08lX (only NV12 / YUY2)", width, height, subtype.Data1);
        }
        return;
    }

    ComPtr<IMFMediaBuffer> buffer;
    DWORD count = 0;
    HRESULT hr = sample->GetBufferCount(&count);
    if (SUCCEEDED(hr)) {
        // Video samples carry one buffer; anything else gets merged (allocates, but is rare).
        hr = count == 1 ? sample->GetBufferByIndex(0, &buffer) : sample->ConvertToContiguousBuffer(&buffer);
    }
    if (FAILED(hr) || !buffer) {
        if (ShouldLogRepeated(bufferFailures_)) Log(L"Renderer: cannot get the sample buffer (0x%08X)", Hex(hr));
        return;
    }

    CpuFrame frame;
    {
        std::lock_guard lock(mutex_);
        std::swap(frame, shared_.spare);  // reuse its memory; the copy below runs unlocked
    }
    auto recycle = [&] {
        std::lock_guard lock(mutex_);
        if (shared_.spare.data.capacity() == 0) std::swap(frame, shared_.spare);
    };

    SubmitInfo info{};
    info.width = width;
    info.height = height;
    info.format = format;
    size_t bufferLength = 0;
    try {
        LockedBuffer locked;
        hr = locked.Lock(buffer.Get());
        if (FAILED(hr)) {
            if (ShouldLogRepeated(bufferFailures_)) Log(L"Renderer: locking the frame buffer failed (0x%08X)", Hex(hr));
            recycle();
            return;
        }
        bufferLength = locked.Length();
        if (const wchar_t* problem = CopyFrame(locked, format, width, height, frame.data, info)) {
            if (ShouldLogRepeated(copyFailures_)) {
                Log(L"Renderer: dropped %ux%u %ls frame: %ls (%ls, pitch %ld, %llu bytes)", width, height,
                    FormatName(format), problem, info.method, long(info.pitch),
                    static_cast<unsigned long long>(bufferLength));
            }
            recycle();
            return;
        }
    } catch (...) {
        // Out of memory for the staging copy; never let it escape into Media Foundation.
        if (ShouldLogRepeated(copyFailures_)) Log(L"Renderer: out of memory copying a %ux%u frame", width, height);
        return;
    }
    frame.width = width;
    frame.height = height;
    frame.format = format;

    bool layoutChanged = false;
    {
        std::lock_guard lock(mutex_);
        std::swap(frame, shared_.pending);
        if (shared_.hasPending) ++shared_.framesReplaced;
        shared_.hasPending = true;
        ++shared_.framesSubmitted;
        // `frame` now holds the replaced buffer: keep it for the next copy.
        if (shared_.spare.data.capacity() == 0) std::swap(frame, shared_.spare);
        if (!(info == shared_.lastSubmit)) {
            shared_.lastSubmit = info;
            layoutChanged = true;
        }
    }
    SetEvent(wakeEvent_);
    if (layoutChanged) {
        Log(L"Renderer: receiving %ux%u %ls via %ls (pitch %ld, buffer %llu bytes)", width, height,
            FormatName(format), info.method, long(info.pitch), static_cast<unsigned long long>(bufferLength));
    }
}

void Renderer::Impl::Clear() {
    {
        std::lock_guard lock(mutex_);
        shared_.clear = true;
        shared_.hasPending = false;
    }
    SetEvent(wakeEvent_);
}

void Renderer::Impl::Resize(UINT width, UINT height) {
    {
        std::lock_guard lock(mutex_);
        shared_.resize = true;
        shared_.width = width;
        shared_.height = height;
    }
    SetEvent(wakeEvent_);
}

void Renderer::Impl::SetPicture(const PictureSettings& picture) {
    {
        std::lock_guard lock(mutex_);
        shared_.picture = picture;
        shared_.pictureChanged = true;
    }
    SetEvent(wakeEvent_);
}

void Renderer::Impl::SetMessage(const std::wstring& text) {
    {
        std::lock_guard lock(mutex_);
        if (shared_.message == text && !shared_.messageChanged) return;
        shared_.message = text;
        shared_.messageChanged = true;
    }
    SetEvent(wakeEvent_);
}

void Renderer::Impl::ShowToast(const std::wstring& text) {
    {
        std::lock_guard lock(mutex_);
        shared_.toast = text;
        shared_.toastStart = Clock::now();
        shared_.toastChanged = true;
    }
    SetEvent(wakeEvent_);
}

void Renderer::Impl::SetVisible(bool visible) {
    {
        std::lock_guard lock(mutex_);
        if (shared_.visible == visible) return;
        shared_.visible = visible;
    }
    Log(L"Renderer: %ls", visible ? L"visible, presenting" : L"hidden, not presenting");
    SetEvent(wakeEvent_);
}

void Renderer::Impl::RequestSnapshot(HWND notify) {
    if (!accepting_.load(std::memory_order_acquire)) {
        Log(L"Renderer: snapshot requested while not running");
        PostMessageW(notify, WM_APP_SNAPSHOT, 0, 0);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (shared_.snapshotTargets.empty()) shared_.snapshotDeadline = Clock::now() + kSnapshotWait;
        shared_.snapshotTargets.push_back(notify);
    }
    SetEvent(wakeEvent_);
}

// ============================================================================================
// Device creation
// ============================================================================================

HRESULT Renderer::Impl::LoadCompiler() {
    if (compile_) return S_OK;
    // Part of Windows 10 and later; prefer System32 so a stray copy elsewhere is not picked up.
    compilerModule_ = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!compilerModule_) compilerModule_ = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compilerModule_) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        return LogIfFailed(hr, L"Renderer: loading d3dcompiler_47.dll");
    }
    // Through void(*)() so the cast does not trip -Wcast-function-type.
    const FARPROC proc = GetProcAddress(compilerModule_, "D3DCompile");
    compile_ = reinterpret_cast<pD3DCompile>(reinterpret_cast<void (*)()>(proc));
    if (!compile_) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        FreeLibrary(compilerModule_);
        compilerModule_ = nullptr;
        return LogIfFailed(hr, L"Renderer: D3DCompile not found in d3dcompiler_47.dll");
    }
    return S_OK;
}

void Renderer::Impl::CreateFactories() {
    if (!d2dFactory_) {
        // Single-threaded: only one thread at a time draws (Initialize, then the render thread).
        D2D1_FACTORY_OPTIONS options{};
        options.debugLevel = D2D1_DEBUG_LEVEL_NONE;
        LogIfFailed(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                      reinterpret_cast<void**>(d2dFactory_.ReleaseAndGetAddressOf())),
                    L"Renderer: D2D1CreateFactory (text overlays disabled)");
    }
    if (!dwrite_) {
        LogIfFailed(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(dwrite_.ReleaseAndGetAddressOf())),
                    L"Renderer: DWriteCreateFactory (text overlays disabled)");
    }
}

HRESULT Renderer::Impl::CreateDeviceResources() {
    HRESULT hr = CreateDevice();
    if (SUCCEEDED(hr)) hr = CreateSwapChain();
    if (SUCCEEDED(hr)) hr = CreatePipeline();
    if (FAILED(hr)) {
        ReleaseDeviceResources();
        return hr;
    }
    if (FAILED(CreateOverlayResources())) Log(L"Renderer: text overlays unavailable on this device");
    UpdatePictureConstants();
    return S_OK;
}

HRESULT Renderer::Impl::CreateDevice() {
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // required for Direct2D interop
    struct Attempt {
        D3D_DRIVER_TYPE type;
        const wchar_t* name;
    };
    const Attempt attempts[] = {{D3D_DRIVER_TYPE_HARDWARE, L"hardware"}, {D3D_DRIVER_TYPE_WARP, L"WARP"}};
    HRESULT hr = E_FAIL;
    for (const Attempt& attempt : attempts) {
        hr = D3D11CreateDevice(nullptr, attempt.type, nullptr, flags, levels, UINT(std::size(levels)),
                               D3D11_SDK_VERSION, device_.ReleaseAndGetAddressOf(), &featureLevel_,
                               context_.ReleaseAndGetAddressOf());
        if (hr == E_INVALIDARG) {
            // Runtimes without 11.1 reject the whole list.
            hr = D3D11CreateDevice(nullptr, attempt.type, nullptr, flags, levels + 1, UINT(std::size(levels) - 1),
                                   D3D11_SDK_VERSION, device_.ReleaseAndGetAddressOf(), &featureLevel_,
                                   context_.ReleaseAndGetAddressOf());
        }
        if (SUCCEEDED(hr)) {
            std::wstring adapterName = L"?";
            DXGI_ADAPTER_DESC desc{};
            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(device_.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
                SUCCEEDED(adapter->GetDesc(&desc))) {
                adapterName = desc.Description;
            }
            Log(L"Renderer: D3D11 %ls device on \"%ls\" (vendor %04X device %04X, %llu MB video memory), feature level %X",
                attempt.name, adapterName.c_str(), desc.VendorId, desc.DeviceId,
                static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024 * 1024)), unsigned(featureLevel_));
            return S_OK;
        }
        Log(L"Renderer: D3D11CreateDevice (%ls) failed (0x%08X)", attempt.name, Hex(hr));
    }
    return hr;
}

HRESULT Renderer::Impl::CreateSwapChain() {
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = device_.As(&dxgiDevice);
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return LogIfFailed(hr, L"Renderer: getting IDXGIFactory2");

    RECT client{};
    GetClientRect(hwnd_, &client);
    const UINT width = UINT(std::max<LONG>(client.right - client.left, 1));
    const UINT height = UINT(std::max<LONG>(client.bottom - client.top, 1));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;  // until a resize lands, stretch rather than crop
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags = kSwapChainFlags;

    ComPtr<IDXGISwapChain1> swapChain1;
    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swapChain1);
    if (FAILED(hr)) {
        Log(L"Renderer: flip-discard swap chain failed (0x%08X); trying flip-sequential", Hex(hr));
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swapChain1);
    }
    if (FAILED(hr)) return LogIfFailed(hr, L"Renderer: CreateSwapChainForHwnd");

    // The app does its own borderless full screen; keep DXGI out of the window's messages.
    LogIfFailed(factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER),
                L"Renderer: MakeWindowAssociation");

    hr = swapChain1.As(&swapChain_);
    if (FAILED(hr)) return LogIfFailed(hr, L"Renderer: IDXGISwapChain2 (needs Windows 8.1+)");
    LogIfFailed(swapChain_->SetMaximumFrameLatency(1), L"Renderer: SetMaximumFrameLatency(1)");
    latencyWaitable_ = swapChain_->GetFrameLatencyWaitableObject();
    if (!latencyWaitable_) Log(L"Renderer: no frame latency waitable object");
    haveLatencyToken_ = false;
    backBufferWidth_ = width;
    backBufferHeight_ = height;
    Log(L"Renderer: swap chain %ux%u, %ls, 2 buffers, max frame latency 1", width, height,
        desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? L"flip-discard" : L"flip-sequential");
    return CreateBackBufferView();
}

HRESULT Renderer::Impl::CreateBackBufferView() {
    // With D3D11 flip model, buffer 0 always names the current back buffer, so one view serves
    // every frame until the next ResizeBuffers.
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &backBufferView_);
    return LogIfFailed(hr, L"Renderer: back buffer render target view");
}

HRESULT Renderer::Impl::CompileShader(const char* body, const char* profile, const wchar_t* name,
                                      ComPtr<ID3DBlob>& blob) {
    std::string source = kShaderCommon;
    source += body;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = compile_(source.data(), source.size(), "CapViewRenderer", nullptr, nullptr, "main", profile,
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.ReleaseAndGetAddressOf(), &errors);
    if (FAILED(hr)) {
        std::string text;
        if (errors) text.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        Log(L"Renderer: compiling the %ls shader (%ls) failed (0x%08X): %ls", name, Utf8ToWide(profile).c_str(),
            Hex(hr), Utf8ToWide(text).c_str());
    }
    return hr;
}

HRESULT Renderer::Impl::CreatePipeline() {
    if (!compile_) return E_UNEXPECTED;
    // vs_5_0/ps_5_0 need feature level 11; the shaders are plain SM4, so 10.x devices work too.
    const bool sm5 = featureLevel_ >= D3D_FEATURE_LEVEL_11_0;
    const char* vsProfile = sm5 ? "vs_5_0" : "vs_4_0";
    const char* psProfile = sm5 ? "ps_5_0" : "ps_4_0";

    ComPtr<ID3DBlob> blob;
    HRESULT hr = CompileShader(kVertexShader, vsProfile, L"vertex", blob);
    if (SUCCEEDED(hr)) hr = device_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vertexShader_);
    if (FAILED(hr)) return LogIfFailed(hr, L"Renderer: vertex shader");

    struct PixelShader {
        const char* source;
        const wchar_t* name;
        ComPtr<ID3D11PixelShader>* shader;
    };
    const PixelShader pixelShaders[] = {
        {kDecodeNV12Shader, L"NV12 decode", &decodeNV12_},
        {kDecodeYUY2Shader, L"YUY2 decode", &decodeYUY2_},
        {kCopyShader, L"copy", &copyShader_},
        {kAdjustShader, L"picture adjustment", &adjustShader_},
    };
    for (const PixelShader& ps : pixelShaders) {
        hr = CompileShader(ps.source, psProfile, ps.name, blob);
        if (SUCCEEDED(hr)) {
            hr = device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                            ps.shader->ReleaseAndGetAddressOf());
        }
        if (FAILED(hr)) return LogIfFailed(hr, L"Renderer: pixel shader");
    }

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sampler, &bilinear_);
    if (FAILED(hr)) return LogIfFailed(hr, L"Renderer: sampler state");

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    hr = device_->CreateRasterizerState(&raster, &rasterizer_);
    if (FAILED(hr)) return LogIfFailed(hr, L"Renderer: rasterizer state");

    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(PictureConstants);
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device_->CreateBuffer(&constants, nullptr, &pictureBuffer_);
    return LogIfFailed(hr, L"Renderer: constant buffer");
}

HRESULT Renderer::Impl::CreateOverlayResources() {
    if (!d2dFactory_ || !dwrite_) return E_NOINTERFACE;
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device_.As(&dxgiDevice);
    if (SUCCEEDED(hr)) hr = d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_);
    if (SUCCEEDED(hr)) hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_);
    if (SUCCEEDED(hr)) hr = d2dContext_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brush_);
    if (FAILED(hr)) {
        LogIfFailed(hr, L"Renderer: Direct2D device context");
        brush_.Reset();
        d2dContext_.Reset();
        d2dDevice_.Reset();
        return hr;
    }
    // Lay out in physical pixels; font sizes are scaled by the window DPI explicitly.
    d2dContext_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    d2dContext_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return S_OK;
}

void Renderer::Impl::ReleaseFrameTextures() {
    lumaView_.Reset();
    lumaTexture_.Reset();
    chromaView_.Reset();
    chromaTexture_.Reset();
    packedView_.Reset();
    packedTexture_.Reset();
    rgbView_.Reset();
    rgbTarget_.Reset();
    rgbTexture_.Reset();
    textureWidth_ = 0;
    textureHeight_ = 0;
    textureFormat_ = PixelFormat::None;
    frameValid_ = false;
}

void Renderer::Impl::ReleaseDeviceResources() {
    if (d2dContext_) d2dContext_->SetTarget(nullptr);
    brush_.Reset();
    d2dContext_.Reset();
    d2dDevice_.Reset();
    ReleaseFrameTextures();
    backBufferView_.Reset();
    vertexShader_.Reset();
    decodeNV12_.Reset();
    decodeYUY2_.Reset();
    copyShader_.Reset();
    adjustShader_.Reset();
    bilinear_.Reset();
    rasterizer_.Reset();
    pictureBuffer_.Reset();
    // Destruction is deferred until the context flushes; a new flip-model swap chain for the
    // same window fails while the old one still exists.
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    if (latencyWaitable_) {
        CloseHandle(latencyWaitable_);
        latencyWaitable_ = nullptr;
    }
    haveLatencyToken_ = false;
    swapChain_.Reset();
    if (context_) context_->Flush();
    context_.Reset();
    device_.Reset();
    backBufferWidth_ = 0;
    backBufferHeight_ = 0;
}

void Renderer::Impl::OnDeviceLost(HRESULT hr, const wchar_t* where) {
    const HRESULT reason = device_ ? device_->GetDeviceRemovedReason() : hr;
    Log(L"Renderer: device lost in %ls (0x%08X, removed reason 0x%08X); recreating", where, Hex(hr), Hex(reason));
    deviceLost_ = true;
    nextRecovery_ = Clock::now();
    ReleaseDeviceResources();
}

bool Renderer::Impl::TryRecoverDevice(Clock::time_point now) {
    if (now < nextRecovery_) return false;
    nextRecovery_ = now + kDeviceRetry;
    const HRESULT hr = CreateDeviceResources();
    if (FAILED(hr)) {
        Log(L"Renderer: recreating the device failed (0x%08X); retrying", Hex(hr));
        return false;
    }
    deviceLost_ = false;
    Log(L"Renderer: device recreated");
    if (current_.format != PixelFormat::None) UploadAndDecode();
    return !deviceLost_;  // the re-upload can find the new device lost already
}

// ============================================================================================
// Render thread
// ============================================================================================

DWORD WINAPI Renderer::Impl::ThreadMain(LPVOID param) {
    auto* self = static_cast<Impl*>(param);
    try {
        self->RenderLoop();
    } catch (...) {
        // Only allocation can throw here; nothing may escape a thread procedure.
        Log(L"Renderer: render thread stopped by an exception (out of memory?)");
    }
    return 0;
}

DWORD Renderer::Impl::NextTimeout(Clock::time_point now) const {
    Clock::duration timeout = kIdleWait;
    if (deviceLost_) timeout = std::min<Clock::duration>(timeout, std::max<Clock::duration>(nextRecovery_ - now, Milliseconds(1)));
    if (toastActive_) {
        const auto elapsed = now - toastStart_;
        timeout = std::min<Clock::duration>(timeout, elapsed < kToastHold ? kToastHold - elapsed : Clock::duration(kFadeStep));
    }
    if (snapshotWaiting_) timeout = std::min<Clock::duration>(timeout, snapshotDeadline_ - now);
    const auto ms = std::chrono::ceil<Milliseconds>(timeout).count();
    return DWORD(std::clamp<long long>(ms, 0, kIdleWait.count()));
}

void Renderer::Impl::RenderLoop() {
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)) {
        Log(L"Renderer: SetThreadPriority failed (%lu)", GetLastError());
    }
    Log(L"Renderer: render thread started");
    bool dirty = true;
    bool visible = true;
    for (;;) {
        WaitForSingleObject(wakeEvent_, NextTimeout(Clock::now()));

        bool clear = false;
        bool resize = false;
        bool pictureChanged = false;
        bool frameReady = false;
        UINT width = 0;
        UINT height = 0;
        const bool wasVisible = visible;
        {
            std::lock_guard lock(mutex_);
            if (shared_.quit) break;
            clear = std::exchange(shared_.clear, false);
            if (std::exchange(shared_.resize, false)) {
                resize = true;
                width = shared_.width;
                height = shared_.height;
            }
            if (std::exchange(shared_.pictureChanged, false)) {
                pictureChanged = true;
                picture_ = shared_.picture;
            }
            if (std::exchange(shared_.messageChanged, false)) {
                message_ = shared_.message;
                dirty = true;
            }
            if (std::exchange(shared_.toastChanged, false)) {
                toast_ = shared_.toast;
                toastStart_ = shared_.toastStart;
                toastActive_ = !toast_.empty();
                dirty = true;
            }
            visible = shared_.visible;
            frameReady = shared_.hasPending;
            snapshotWaiting_ = !shared_.snapshotTargets.empty();
            snapshotDeadline_ = shared_.snapshotDeadline;
        }
        const Clock::time_point now = Clock::now();
        if (visible && !wasVisible) dirty = true;
        if (clear) {
            frameValid_ = false;
            current_.format = PixelFormat::None;  // keep the memory, forget the picture
            dirty = true;
        }

        if (deviceLost_) {
            if (!TryRecoverDevice(now)) {
                ServeSnapshots(false);  // answers with nullptr once the deadline passes
                continue;
            }
            dirty = true;
            pictureChanged = true;
        }
        if (resize) {
            ResizeSwapChain(width, height);
            dirty = true;
        }
        if (pictureChanged) {
            UpdatePictureConstants();
            dirty = true;
        }
        if (toastActive_) {
            const auto elapsed = now - toastStart_;
            if (elapsed >= kToastHold + kToastFade) {
                toastActive_ = false;
                dirty = true;  // one more frame without it
            } else if (elapsed >= kToastHold) {
                dirty = true;  // fading
            }
        }

        // Wait for the swap chain before taking the frame, so the newest one gets drawn.
        const bool wantFrame = frameReady && (visible || snapshotWaiting_);
        if (visible && (dirty || wantFrame) && !deviceLost_) WaitForSwapChain();
        bool decoded = false;
        if (wantFrame && !deviceLost_ && TakePendingFrame()) decoded = UploadAndDecode();
        if (decoded) dirty = true;
        if (snapshotWaiting_) ServeSnapshots(decoded);
        if (visible && dirty && !deviceLost_) {
            PresentScene(Clock::now());  // after the swap chain wait, so the toast fade is current
            dirty = false;
        }

        if (now - lastStats_ >= kStatsInterval) LogStats();
    }
    Log(L"Renderer: render thread exiting");
}

void Renderer::Impl::LogStats() {
    uint64_t submitted = 0;
    uint64_t replaced = 0;
    {
        std::lock_guard lock(mutex_);
        submitted = shared_.framesSubmitted;
        replaced = shared_.framesReplaced;
    }
    lastStats_ = Clock::now();
    if (submitted == statsSubmitted_) return;  // idle: nothing worth a line
    statsSubmitted_ = submitted;
    Log(L"Renderer: totals %llu frames received, %llu replaced by newer ones, %llu decoded, %llu presented",
        static_cast<unsigned long long>(submitted), static_cast<unsigned long long>(replaced),
        static_cast<unsigned long long>(framesDecoded_), static_cast<unsigned long long>(framesPresented_));
}

void Renderer::Impl::ResizeSwapChain(UINT width, UINT height) {
    if (!swapChain_ || width == 0 || height == 0) return;  // minimized: keep the old buffers
    if (width == backBufferWidth_ && height == backBufferHeight_) return;
    // ResizeBuffers fails while anything still references a back buffer.
    if (d2dContext_) d2dContext_->SetTarget(nullptr);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    backBufferView_.Reset();
    context_->Flush();
    const HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, kSwapChainFlags);
    if (IsDeviceLost(hr)) {
        OnDeviceLost(hr, L"ResizeBuffers");
        return;
    }
    if (FAILED(hr)) {
        Log(L"Renderer: ResizeBuffers(%ux%u) failed (0x%08X); keeping %ux%u", width, height, Hex(hr),
            backBufferWidth_, backBufferHeight_);
    } else {
        backBufferWidth_ = width;
        backBufferHeight_ = height;
    }
    if (FAILED(CreateBackBufferView())) OnDeviceLost(E_FAIL, L"CreateBackBufferView");
}

void Renderer::Impl::UpdatePictureConstants() {
    pictureNeutral_ = picture_.IsNeutral();
    if (!context_ || !pictureBuffer_) return;
    const PictureConstants constants = MakePictureConstants(picture_);
    context_->UpdateSubresource(pictureBuffer_.Get(), 0, nullptr, &constants, 0, 0);
}

void Renderer::Impl::WaitForSwapChain() {
    if (haveLatencyToken_ || !latencyWaitable_) return;
    const DWORD result = WaitForSingleObjectEx(latencyWaitable_, DWORD(kLatencyWait.count()), FALSE);
    if (result != WAIT_OBJECT_0 && ShouldLogRepeated(latencyTimeouts_)) {
        Log(L"Renderer: swap chain not ready after %lld ms (wait result %lu); presenting anyway",
            static_cast<long long>(kLatencyWait.count()), result);
    }
    // Held until the next Present, so a pass that ends up not presenting does not cost a wait.
    haveLatencyToken_ = true;
}

bool Renderer::Impl::TakePendingFrame() {
    std::lock_guard lock(mutex_);
    if (!shared_.hasPending) return false;
    std::swap(current_, shared_.pending);  // `pending` keeps the old buffer for the producer to reuse
    shared_.hasPending = false;
    return true;
}

bool Renderer::Impl::EnsureFrameTextures(UINT width, UINT height, PixelFormat format) {
    if (rgbTexture_ && width == textureWidth_ && height == textureHeight_ && format == textureFormat_) return true;
    ReleaseFrameTextures();

    auto makeTexture = [&](UINT w, UINT h, DXGI_FORMAT dxgiFormat, UINT bind, ComPtr<ID3D11Texture2D>& texture,
                           ComPtr<ID3D11ShaderResourceView>& view) -> HRESULT {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = dxgiFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = bind;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(texture.Get(), nullptr, &view);
        return hr;
    };

    HRESULT hr = S_OK;
    if (format == PixelFormat::NV12) {
        hr = makeTexture(width, height, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, lumaTexture_, lumaView_);
        if (SUCCEEDED(hr)) {
            hr = makeTexture(ChromaWidth(width), ChromaHeight(height), DXGI_FORMAT_R8G8_UNORM,
                             D3D11_BIND_SHADER_RESOURCE, chromaTexture_, chromaView_);
        }
    } else {
        hr = makeTexture(ChromaWidth(width), height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                         packedTexture_, packedView_);
    }
    if (SUCCEEDED(hr)) {
        hr = makeTexture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, rgbTexture_, rgbView_);
    }
    if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(rgbTexture_.Get(), nullptr, &rgbTarget_);
    if (FAILED(hr)) {
        if (ShouldLogRepeated(textureFailures_)) {
            Log(L"Renderer: creating %ux%u %ls textures failed (0x%08X)", width, height, FormatName(format), Hex(hr));
        }
        ReleaseFrameTextures();
        if (FAILED(device_->GetDeviceRemovedReason())) OnDeviceLost(hr, L"CreateTexture2D");
        return false;
    }
    textureWidth_ = width;
    textureHeight_ = height;
    textureFormat_ = format;
    Log(L"Renderer: frame textures %ux%u %ls", width, height, FormatName(format));
    return true;
}

bool Renderer::Impl::UploadAndDecode() {
    const CpuFrame& frame = current_;
    if (frame.format == PixelFormat::None || frame.data.size() < FrameBytes(frame.format, frame.width, frame.height)) {
        return false;
    }
    if (!EnsureFrameTextures(frame.width, frame.height, frame.format)) return false;

    const uint8_t* data = frame.data.data();
    if (frame.format == PixelFormat::NV12) {
        context_->UpdateSubresource(lumaTexture_.Get(), 0, nullptr, data, frame.width, 0);
        context_->UpdateSubresource(chromaTexture_.Get(), 0, nullptr, data + size_t(frame.width) * frame.height,
                                    ChromaWidth(frame.width) * 2, 0);
    } else {
        context_->UpdateSubresource(packedTexture_.Get(), 0, nullptr, data, ChromaWidth(frame.width) * 4, 0);
    }

    // Pass 1: YUV -> gamma-encoded RGBA at source size. Start from a clean pipeline: Direct2D
    // draws through the same context.
    context_->ClearState();
    ID3D11RenderTargetView* target = rgbTarget_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    const D3D11_VIEWPORT viewport = FullViewport(frame.width, frame.height);
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_.Get());
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    if (frame.format == PixelFormat::NV12) {
        ID3D11ShaderResourceView* views[2] = {lumaView_.Get(), chromaView_.Get()};
        context_->PSSetShader(decodeNV12_.Get(), nullptr, 0);
        context_->PSSetShaderResources(0, 2, views);
    } else {
        ID3D11ShaderResourceView* views[2] = {packedView_.Get(), nullptr};
        context_->PSSetShader(decodeYUY2_.Get(), nullptr, 0);
        context_->PSSetShaderResources(0, 2, views);
    }
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* none[2] = {};
    context_->PSSetShaderResources(0, 2, none);
    context_->OMSetRenderTargets(0, nullptr, nullptr);

    frameValid_ = true;
    ++framesDecoded_;
    return true;
}

void Renderer::Impl::DrawPicture(const D3D11_VIEWPORT& viewport) {
    // Pass 2: the decoded picture, bilinear, with the picture adjustments unless neutral.
    // All state is set every time because Direct2D draws through the same context.
    ID3D11ShaderResourceView* view = rgbView_.Get();
    ID3D11SamplerState* sampler = bilinear_.Get();
    ID3D11Buffer* constants = pictureBuffer_.Get();
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_.Get());
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pictureNeutral_ ? copyShader_.Get() : adjustShader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, &view);
    context_->PSSetSamplers(0, 1, &sampler);
    context_->PSSetConstantBuffers(0, 1, &constants);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    context_->PSSetShaderResources(0, 1, &none);
}

void Renderer::Impl::DrawScene() {
    context_->ClearState();
    ID3D11RenderTargetView* target = backBufferView_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    // Flip-discard leaves the back buffer undefined: always redraw everything.
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context_->ClearRenderTargetView(target, black);
    if (frameValid_) DrawPicture(Letterbox(backBufferWidth_, backBufferHeight_, textureWidth_, textureHeight_));
    context_->OMSetRenderTargets(0, nullptr, nullptr);
}

void Renderer::Impl::EnsureTextFormats() {
    UINT dpi = GetDpiForWindow(hwnd_);
    if (dpi == 0) dpi = 96;
    if (dpi == formatDpi_ && messageFormat_ && toastFormat_) return;
    messageFormat_.Reset();
    toastFormat_.Reset();
    messageLayout_.Reset();
    toastLayout_.Reset();
    formatDpi_ = dpi;
    const float scale = float(dpi) / 96.0f;
    // DirectWrite falls back per character if Malgun Gothic were ever missing.
    HRESULT hr = dwrite_->CreateTextFormat(L"Malgun Gothic", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f * scale,
                                           L"ko-KR", &messageFormat_);
    if (SUCCEEDED(hr)) {
        messageFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        messageFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        messageFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        hr = dwrite_->CreateTextFormat(L"Malgun Gothic", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f * scale, L"ko-KR",
                                       &toastFormat_);
    }
    if (SUCCEEDED(hr)) {
        toastFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        toastFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        toastFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        Log(L"Renderer: text formats for %u dpi", dpi);
    } else {
        LogIfFailed(hr, L"Renderer: CreateTextFormat");
        messageFormat_.Reset();
        toastFormat_.Reset();
    }
}

void Renderer::Impl::DrawOverlays(Clock::time_point now) {
    float toastAlpha = 0.0f;
    if (toastActive_ && !toast_.empty()) {
        const auto elapsed = now - toastStart_;
        if (elapsed < kToastHold) {
            toastAlpha = 1.0f;
        } else {
            const float t = std::clamp(std::chrono::duration<float>(elapsed - kToastHold).count() /
                                           std::chrono::duration<float>(kToastFade).count(),
                                       0.0f, 1.0f);
            toastAlpha = 1.0f - t * t * (3.0f - 2.0f * t);  // ease in-out, like a Core Animation fade
        }
    }
    const bool showMessage = !message_.empty();
    const bool showToast = toastAlpha > 0.0f;
    if ((!showMessage && !showToast) || !d2dContext_ || !brush_ || !dwrite_) return;
    EnsureTextFormats();
    if (!messageFormat_ || !toastFormat_) return;

    const float scale = float(formatDpi_) / 96.0f;
    const float width = float(backBufferWidth_);
    const float height = float(backBufferHeight_);

    if (showMessage) {
        const float maxWidth = std::max(width - 48.0f * scale, 1.0f);
        if (!messageLayout_ || messageLayoutText_ != message_ || messageLayoutWidth_ != maxWidth) {
            messageLayout_.Reset();
            const HRESULT hr = dwrite_->CreateTextLayout(message_.c_str(), UINT32(message_.size()),
                                                         messageFormat_.Get(), maxWidth, 100000.0f, &messageLayout_);
            if (FAILED(hr)) {
                messageLayout_.Reset();
                LogIfFailed(hr, L"Renderer: message text layout");
            }
            messageLayoutText_ = message_;
            messageLayoutWidth_ = maxWidth;
        }
    }
    if (showToast && (!toastLayout_ || toastLayoutText_ != toast_)) {
        toastLayout_.Reset();
        const HRESULT hr = dwrite_->CreateTextLayout(toast_.c_str(), UINT32(toast_.size()), toastFormat_.Get(),
                                                     100000.0f, 100000.0f, &toastLayout_);
        if (FAILED(hr)) {
            toastLayout_.Reset();
            LogIfFailed(hr, L"Renderer: toast text layout");
        }
        toastLayoutText_ = toast_;
    }

    // A target bitmap per frame over the current back buffer, released right after drawing so
    // nothing outlives the frame (ResizeBuffers needs every reference gone).
    ComPtr<IDXGISurface> surface;
    ComPtr<ID2D1Bitmap1> bitmap;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (SUCCEEDED(hr)) {
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        hr = d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &bitmap);
    }
    if (FAILED(hr)) {
        if (IsDeviceLost(hr)) {
            OnDeviceLost(hr, L"CreateBitmapFromDxgiSurface");
        } else if (ShouldLogRepeated(overlayFailures_)) {
            Log(L"Renderer: overlay target failed (0x%08X)", Hex(hr));
        }
        return;
    }

    d2dContext_->SetTarget(bitmap.Get());
    d2dContext_->BeginDraw();
    d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());

    if (showMessage && messageLayout_) {
        DWRITE_TEXT_METRICS metrics{};
        messageLayout_->GetMetrics(&metrics);
        const float originX = std::floor((width - messageLayoutWidth_) / 2.0f);
        const float originY = std::floor((height - metrics.height) / 2.0f);
        const float padX = 20.0f * scale;
        const float padY = 14.0f * scale;
        const D2D1_RECT_F box = D2D1::RectF(originX + metrics.left - padX, originY + metrics.top - padY,
                                            originX + metrics.left + metrics.width + padX,
                                            originY + metrics.top + metrics.height + padY);
        brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.5f));
        d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(box, 10.0f * scale, 10.0f * scale), brush_.Get());
        brush_->SetColor(D2D1::ColorF(0.85f, 0.85f, 0.85f, 1.0f));
        d2dContext_->DrawTextLayout(D2D1::Point2F(originX, originY), messageLayout_.Get(), brush_.Get());
    }

    if (showToast && toastLayout_) {
        DWRITE_TEXT_METRICS metrics{};
        toastLayout_->GetMetrics(&metrics);
        const float pillHeight = std::max(32.0f * scale, metrics.height + 10.0f * scale);
        const float pillWidth = std::min(std::max(140.0f * scale, metrics.width + 32.0f * scale),
                                         std::max(width - 16.0f * scale, 1.0f));
        const float left = std::floor((width - pillWidth) / 2.0f);
        const float bottom = height - 32.0f * scale;
        const float top = bottom - pillHeight;
        const D2D1_RECT_F pill = D2D1::RectF(left, top, left + pillWidth, bottom);
        brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f * toastAlpha));
        d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(pill, 8.0f * scale, 8.0f * scale), brush_.Get());
        brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, toastAlpha));
        d2dContext_->PushAxisAlignedClip(pill, D2D1_ANTIALIAS_MODE_ALIASED);
        const float textX = std::floor(left + (pillWidth - metrics.width) / 2.0f - metrics.left);
        const float textY = std::floor(top + (pillHeight - metrics.height) / 2.0f - metrics.top);
        d2dContext_->DrawTextLayout(D2D1::Point2F(textX, textY), toastLayout_.Get(), brush_.Get());
        d2dContext_->PopAxisAlignedClip();
    }

    hr = d2dContext_->EndDraw();
    d2dContext_->SetTarget(nullptr);
    if (IsDeviceLost(hr)) {
        OnDeviceLost(hr, L"Direct2D EndDraw");
    } else if (FAILED(hr) && ShouldLogRepeated(overlayFailures_)) {
        Log(L"Renderer: Direct2D EndDraw failed (0x%08X)", Hex(hr));
    }
}

void Renderer::Impl::PresentScene(Clock::time_point now) {
    if (!swapChain_ || !backBufferView_) return;
    DrawScene();
    DrawOverlays(now);
    if (deviceLost_) return;
    // Sync interval 0: on a flip-model swap chain a newer frame replaces one still queued.
    const HRESULT hr = swapChain_->Present(0, 0);
    haveLatencyToken_ = false;
    if (IsDeviceLost(hr)) {
        OnDeviceLost(hr, L"Present");
        return;
    }
    if (FAILED(hr)) {
        if (ShouldLogRepeated(presentFailures_)) Log(L"Renderer: Present failed (0x%08X)", Hex(hr));
        return;
    }
    ++framesPresented_;
}

void Renderer::Impl::ServeSnapshots(bool freshFrame) {
    std::vector<HWND> targets;
    {
        std::lock_guard lock(mutex_);
        if (shared_.snapshotTargets.empty()) return;
        // Wait for the next frame, up to the deadline; then take whatever is on screen.
        if (!freshFrame && Clock::now() < shared_.snapshotDeadline) return;
        targets.swap(shared_.snapshotTargets);
    }
    snapshotWaiting_ = false;

    std::unique_ptr<SnapshotImage> image;
    if (frameValid_ && !deviceLost_) {
        try {
            image = RenderSnapshot();
        } catch (const std::exception&) {
            Log(L"Renderer: out of memory for a %ux%u snapshot", textureWidth_, textureHeight_);
            image.reset();
        }
    } else {
        Log(L"Renderer: snapshot requested but there is no picture");
    }
    for (size_t i = 0; i < targets.size(); ++i) {
        SnapshotImage* result = nullptr;
        if (image) {
            try {
                result = i + 1 == targets.size() ? image.release() : new SnapshotImage(*image);
            } catch (const std::exception&) {
                result = nullptr;
            }
        }
        if (!PostMessageW(targets[i], WM_APP_SNAPSHOT, 0, reinterpret_cast<LPARAM>(result))) {
            Log(L"Renderer: posting the snapshot failed (%lu)", GetLastError());
            delete result;
        }
    }
}

std::unique_ptr<SnapshotImage> Renderer::Impl::RenderSnapshot() {
    const UINT width = textureWidth_;
    const UINT height = textureHeight_;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
    if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(texture.Get(), nullptr, &target);
    if (SUCCEEDED(hr)) {
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device_->CreateTexture2D(&desc, nullptr, &staging);
    }
    if (FAILED(hr)) {
        LogIfFailed(hr, L"Renderer: snapshot textures");
        return nullptr;
    }

    // The same pass 2 as on screen, at source size (1:1, so bilinear sampling is exact).
    context_->ClearState();
    ID3D11RenderTargetView* rtv = target.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    DrawPicture(FullViewport(width, height));
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    context_->CopyResource(staging.Get(), texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        if (IsDeviceLost(hr)) {
            OnDeviceLost(hr, L"snapshot Map");
        } else {
            LogIfFailed(hr, L"Renderer: mapping the snapshot");
        }
        return nullptr;
    }
    std::unique_ptr<SnapshotImage> image;
    try {
        image = std::make_unique<SnapshotImage>();
        image->width = width;
        image->height = height;
        image->bgra.resize(size_t(width) * height * 4);
    } catch (...) {
        context_->Unmap(staging.Get(), 0);
        throw;
    }
    for (UINT y = 0; y < height; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
        uint8_t* dst = image->bgra.data() + size_t(y) * width * 4;
        for (UINT x = 0; x < width; ++x, src += 4, dst += 4) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 255;
        }
    }
    context_->Unmap(staging.Get(), 0);
    Log(L"Renderer: snapshot %ux%u%ls", width, height, pictureNeutral_ ? L"" : L" with picture adjustments");
    return image;
}

// ============================================================================================
// Renderer
// ============================================================================================

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}

Renderer::~Renderer() = default;

HRESULT Renderer::Initialize(HWND hwnd) { return impl_->Initialize(hwnd); }

void Renderer::Shutdown() { impl_->Shutdown(); }

void Renderer::SubmitFrame(IMFSample* sample, UINT32 width, UINT32 height, const GUID& subtype) {
    impl_->Submit(sample, width, height, subtype);
}

void Renderer::ClearFrame() { impl_->Clear(); }

void Renderer::Resize(UINT width, UINT height) { impl_->Resize(width, height); }

void Renderer::SetPicture(const PictureSettings& picture) { impl_->SetPicture(picture); }

void Renderer::SetMessage(const std::wstring& text) { impl_->SetMessage(text); }

void Renderer::ShowToast(const std::wstring& text) { impl_->ShowToast(text); }

void Renderer::SetVisible(bool visible) { impl_->SetVisible(visible); }

void Renderer::RequestSnapshot(HWND notify) { impl_->RequestSnapshot(notify); }
