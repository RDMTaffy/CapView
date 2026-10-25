// The modeless "화면 조정" window: live sliders for the picture corrections.
#pragma once

#include "Common.h"

class PictureDialog {
public:
    using ChangeHandler = std::function<void(const PictureSettings&)>;

    PictureDialog();
    ~PictureDialog();
    PictureDialog(const PictureDialog&) = delete;
    PictureDialog& operator=(const PictureDialog&) = delete;

    /// Shows (or brings forward) the window, owned by `owner`, initialised with `current`.
    /// `onChange` runs on the UI thread on every change, including while a slider is dragged.
    void Show(HWND owner, const PictureSettings& current, ChangeHandler onChange);
    void Close();
    bool IsOpen() const;

    /// Call from the message loop so Tab / keyboard work inside the window.
    bool PreTranslate(MSG* msg);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
