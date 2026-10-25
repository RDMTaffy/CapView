// Puts a captured frame on the clipboard as PNG (registered "PNG" format) and CF_DIB.
#pragma once

#include "Common.h"

/// UI thread. Returns false (and logs why) if the clipboard could not be written.
bool CopyImageToClipboard(HWND owner, const SnapshotImage& image);
