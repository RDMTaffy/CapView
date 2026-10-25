// Screen capture to the clipboard: PNG (registered "PNG" format, what browsers, Discord and
// Office prefer) plus CF_DIB for everything else. Windows synthesises CF_BITMAP / CF_DIBV5.
#include "Clipboard.h"

#include <objbase.h>
#include <wincodec.h>

#include <chrono>
#include <cstring>

namespace {

constexpr UINT kMaxSide = 16384;  // keeps every byte count below 4 GiB (DWORD / UINT)

// Owns an HGLOBAL until the clipboard takes it.
class GlobalMemory {
public:
    GlobalMemory() = default;
    explicit GlobalMemory(HGLOBAL handle) : handle_(handle) {}
    ~GlobalMemory() {
        if (handle_) GlobalFree(handle_);
    }
    GlobalMemory(GlobalMemory&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    GlobalMemory& operator=(GlobalMemory&& other) noexcept {
        if (this != &other) {
            if (handle_) GlobalFree(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    GlobalMemory(const GlobalMemory&) = delete;
    GlobalMemory& operator=(const GlobalMemory&) = delete;

    HGLOBAL Get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }
    HGLOBAL Release() {
        HGLOBAL handle = handle_;
        handle_ = nullptr;
        return handle;
    }

private:
    HGLOBAL handle_ = nullptr;
};

// Balanced CoInitializeEx: S_FALSE on the (already initialised) UI thread still needs a
// matching CoUninitialize; RPC_E_CHANGED_MODE means COM is usable but not ours to undo.
// Declared before any ComPtr so it is destroyed after them.
class ComScope {
public:
    ComScope() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    HRESULT hr_;
};

HRESULT EncodePng(const SnapshotImage& image, GlobalMemory& out) {
    ComScope com;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = LogIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&factory)),
                             L"Clipboard: create WIC factory");
    if (FAILED(hr)) return hr;

    // The stream owns (and frees) its growing HGLOBAL; the result is copied into an
    // exactly-sized block because that HGLOBAL may be larger than the PNG.
    ComPtr<IStream> stream;
    hr = LogIfFailed(CreateStreamOnHGlobal(nullptr, TRUE, &stream), L"Clipboard: CreateStreamOnHGlobal");
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapEncoder> encoder;
    hr = LogIfFailed(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), L"Clipboard: create PNG encoder");
    if (FAILED(hr)) return hr;
    hr = LogIfFailed(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), L"Clipboard: PNG encoder Initialize");
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    hr = LogIfFailed(encoder->CreateNewFrame(&frame, &options), L"Clipboard: PNG CreateNewFrame");
    if (FAILED(hr)) return hr;
    hr = LogIfFailed(frame->Initialize(options.Get()), L"Clipboard: PNG frame Initialize");
    if (FAILED(hr)) return hr;
    hr = LogIfFailed(frame->SetSize(image.width, image.height), L"Clipboard: PNG frame SetSize");
    if (FAILED(hr)) return hr;

    // A screenshot is opaque: describe the source as 32bppBGR (alpha byte ignored) and ask for
    // 24-bit PNG, so a renderer that leaves alpha at 0 cannot produce an invisible image.
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    hr = LogIfFailed(frame->SetPixelFormat(&format), L"Clipboard: PNG SetPixelFormat");
    if (FAILED(hr)) return hr;

    const UINT stride = image.width * 4;
    const UINT bytes = stride * image.height;
    // CreateBitmapFromMemory copies the pixels, so the const_cast never leads to a write.
    ComPtr<IWICBitmap> bitmap;
    hr = LogIfFailed(factory->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppBGR, stride,
                                                     bytes, const_cast<BYTE*>(image.bgra.data()), &bitmap),
                     L"Clipboard: CreateBitmapFromMemory");
    if (FAILED(hr)) return hr;

    // Explicit converter to whatever format the encoder settled on: WriteSource converts by
    // itself only on newer Windows versions.
    ComPtr<IWICFormatConverter> converter;
    hr = LogIfFailed(factory->CreateFormatConverter(&converter), L"Clipboard: CreateFormatConverter");
    if (FAILED(hr)) return hr;
    hr = LogIfFailed(converter->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                           WICBitmapPaletteTypeCustom),
                     L"Clipboard: format converter Initialize");
    if (FAILED(hr)) return hr;

    hr = LogIfFailed(frame->WriteSource(converter.Get(), nullptr), L"Clipboard: PNG WriteSource");
    if (FAILED(hr)) return hr;
    hr = LogIfFailed(frame->Commit(), L"Clipboard: PNG frame Commit");
    if (FAILED(hr)) return hr;
    hr = LogIfFailed(encoder->Commit(), L"Clipboard: PNG encoder Commit");
    if (FAILED(hr)) return hr;

    STATSTG stat{};
    hr = LogIfFailed(stream->Stat(&stat, STATFLAG_NONAME), L"Clipboard: PNG stream Stat");
    if (FAILED(hr)) return hr;
    const ULONGLONG size = stat.cbSize.QuadPart;
    if (size == 0 || size > 0xFFFFFFFFull) {
        Log(L"Clipboard: PNG stream has unexpected size %llu", size);
        return E_UNEXPECTED;
    }

    HGLOBAL streamMemory = nullptr;
    hr = LogIfFailed(GetHGlobalFromStream(stream.Get(), &streamMemory), L"Clipboard: GetHGlobalFromStream");
    if (FAILED(hr)) return hr;
    if (GlobalSize(streamMemory) < size) {
        Log(L"Clipboard: PNG stream memory (%llu bytes) is smaller than the stream (%llu bytes)",
            static_cast<unsigned long long>(GlobalSize(streamMemory)), size);
        return E_UNEXPECTED;
    }

    GlobalMemory png(GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(size)));
    if (!png) return LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Clipboard: GlobalAlloc for PNG");
    void* source = GlobalLock(streamMemory);
    void* target = GlobalLock(png.Get());
    if (source && target) std::memcpy(target, source, static_cast<size_t>(size));
    if (target) GlobalUnlock(png.Get());
    if (source) GlobalUnlock(streamMemory);
    if (!source || !target) return LogIfFailed(E_OUTOFMEMORY, L"Clipboard: GlobalLock for PNG");

    out = std::move(png);
    return S_OK;
}

// BITMAPINFOHEADER + bottom-up rows, 32-bit BI_RGB. The fourth byte is formally unused, but
// some programs read it as alpha, so it is forced to 255 (opaque).
HRESULT MakeDib(const SnapshotImage& image, GlobalMemory& out) {
    const size_t rowBytes = size_t(image.width) * 4;
    const size_t pixelBytes = rowBytes * image.height;
    GlobalMemory dib(GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixelBytes));
    if (!dib) return LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Clipboard: GlobalAlloc for DIB");
    auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(dib.Get()));
    if (!header) return LogIfFailed(E_OUTOFMEMORY, L"Clipboard: GlobalLock for DIB");

    *header = BITMAPINFOHEADER{};
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = static_cast<LONG>(image.width);
    header->biHeight = static_cast<LONG>(image.height);  // positive = bottom-up
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(pixelBytes);

    uint8_t* pixels = reinterpret_cast<uint8_t*>(header + 1);
    for (UINT y = 0; y < image.height; ++y) {
        uint8_t* row = pixels + size_t(y) * rowBytes;
        std::memcpy(row, image.bgra.data() + size_t(image.height - 1 - y) * rowBytes, rowBytes);
        for (size_t x = 3; x < rowBytes; x += 4) row[x] = 0xFF;
    }
    GlobalUnlock(dib.Get());

    out = std::move(dib);
    return S_OK;
}

// Another program (often a clipboard manager) may hold the clipboard for a moment.
bool OpenClipboardWithRetry(HWND owner) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(owner)) return true;
        Sleep(15);
    }
    return false;
}

}  // namespace

bool CopyImageToClipboard(HWND owner, const SnapshotImage& image) {
    const auto started = std::chrono::steady_clock::now();

    if (image.width == 0 || image.height == 0 || image.width > kMaxSide || image.height > kMaxSide) {
        Log(L"Clipboard: refusing image of %ux%u", image.width, image.height);
        return false;
    }
    const size_t needed = size_t(image.width) * image.height * 4;
    if (image.bgra.size() < needed) {
        Log(L"Clipboard: image %ux%u has %zu bytes, expected %zu", image.width, image.height, image.bgra.size(), needed);
        return false;
    }
    if (!owner) Log(L"Clipboard: no owner window; SetClipboardData may fail");

    // Build both payloads before opening the clipboard so it stays locked only briefly.
    // Either one alone is still worth putting on the clipboard.
    GlobalMemory png;
    const HRESULT pngResult = EncodePng(image, png);
    GlobalMemory dib;
    const HRESULT dibResult = MakeDib(image, dib);
    if (!png && !dib) {
        Log(L"Clipboard: nothing to copy (PNG hr=0x%08lX, DIB hr=0x%08lX)", static_cast<unsigned long>(pngResult),
            static_cast<unsigned long>(dibResult));
        return false;
    }
    const SIZE_T pngBytes = png ? GlobalSize(png.Get()) : 0;

    const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    if (!pngFormat) LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Clipboard: RegisterClipboardFormat(PNG)");

    if (!OpenClipboardWithRetry(owner)) {
        LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Clipboard: OpenClipboard");
        HWND holder = GetOpenClipboardWindow();
        DWORD holderPid = 0;
        if (holder) GetWindowThreadProcessId(holder, &holderPid);
        Log(L"Clipboard: held by window %p (pid %lu)", static_cast<void*>(holder), static_cast<unsigned long>(holderPid));
        return false;
    }
    if (!EmptyClipboard()) {
        LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Clipboard: EmptyClipboard");
        CloseClipboard();
        return false;
    }

    // On success the system owns the memory; on failure it is still ours to free.
    bool pngSet = false;
    bool dibSet = false;
    if (png && pngFormat) {
        if (SetClipboardData(pngFormat, png.Get())) {
            png.Release();
            pngSet = true;
        } else {
            LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Clipboard: SetClipboardData(PNG)");
        }
    }
    if (dib) {
        if (SetClipboardData(CF_DIB, dib.Get())) {
            dib.Release();
            dibSet = true;
        } else {
            LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Clipboard: SetClipboardData(CF_DIB)");
        }
    }
    if (!CloseClipboard()) LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"Clipboard: CloseClipboard");

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    Log(L"Clipboard: %ux%u copied in %lld ms (PNG %ls, %llu bytes; DIB %ls)", image.width, image.height,
        static_cast<long long>(elapsed), pngSet ? L"ok" : L"missing", static_cast<unsigned long long>(pngSet ? pngBytes : 0),
        dibSet ? L"ok" : L"missing");
    return pngSet || dibSet;
}
