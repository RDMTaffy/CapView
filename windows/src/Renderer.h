// Direct3D 11 presentation: newest frame, letterboxed, picture adjustments, text overlays.
#pragma once

#include "Common.h"

#include <mfidl.h>

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Creates the device and a flip-model swap chain for `hwnd` and starts the render thread.
    HRESULT Initialize(HWND hwnd);
    void Shutdown();

    /// Any thread. Keeps only the newest frame; the render thread draws it as soon as possible.
    /// `subtype` is MFVideoFormat_NV12 or MFVideoFormat_YUY2 (8-bit, BT.709 limited range).
    void SubmitFrame(IMFSample* sample, UINT32 width, UINT32 height, const GUID& subtype);

    /// Drops the current picture (window shows black).
    void ClearFrame();

    /// UI thread: new client size in pixels.
    void Resize(UINT width, UINT height);

    void SetPicture(const PictureSettings& picture);

    /// Centered multi-line message over the video area (empty hides it).
    void SetMessage(const std::wstring& text);

    /// Short status line near the bottom, fades after about 1.2 s.
    void ShowToast(const std::wstring& text);

    /// False while minimized/hidden: frames are not presented.
    void SetVisible(bool visible);

    /// Renders the next frame (or the current one if no new frame arrives within 100 ms) at its
    /// source resolution with the picture adjustments applied, then posts
    /// WM_APP_SNAPSHOT(lParam = new SnapshotImage, or nullptr if there is no frame) to `notify`.
    void RequestSnapshot(HWND notify);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
