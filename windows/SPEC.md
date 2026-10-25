# CapView for Windows — implementation spec

Native Win32 + C++20 port of the macOS app in `../Sources`. Same features, same Korean UI
strings, same behaviour. Built with mingw-w64 (cross, from macOS) and must also compile
with MSVC (no GNU-only extensions; no MSVC-only ones either).

## Build / compile check

```
x86_64-w64-mingw32-g++ -std=c++20 -O2 -municode -DUNICODE -D_UNICODE -DNOMINMAX \
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wall -Wextra -Iwindows/src -c windows/src/<File>.cpp -o /tmp/<File>.o
```

Header order quirk on mingw: include `<mfidl.h>` before `<mfreadwrite.h>`.
Link libs: mfplat mfreadwrite mf mfuuid d3d11 dxgi d3dcompiler d2d1 dwrite windowscodecs
avrt cfgmgr32 setupapi comctl32 propsys imm32 ole32 oleaut32 uuid shlwapi winmm user32 gdi32.
mfplat, mf and mfreadwrite are delay-loaded (build.sh makes delay-import libraries from
`windows/delayload/*.def`; with MSVC use `/DELAYLOAD` and delayimp.lib), so Windows N/KN without
the Media Feature Pack reaches main.cpp's message. An MSVC project must link with `/MANIFEST:NO`:
`res/app.rc` already embeds `app.manifest`, and a second linker manifest fails with CVT1100.

## Threads

- **UI thread**: Win32 message loop, `App`, settings, menus, `PictureDialog`, clipboard. COM STA.
- **Media Foundation worker threads**: `VideoCapture` async reader callbacks → `FrameSink`
  → `Renderer::SubmitFrame` (must be cheap and thread-safe: keep newest frame only).
- **Render thread** (owned by `Renderer`): the only user of the D3D11 immediate context.
- **Two audio threads** (owned by `AudioPassthrough`): event-driven WASAPI capture and render,
  MMCSS "Pro Audio" (`AvSetMmThreadCharacteristicsW`). No allocation, locks or logging on
  the audio threads' hot path; they share only a lock-free SPSC ring and atomics.
- Everything reports back to the UI thread with `PostMessage` (see `Common.h`).

## Logging

Every module logs start/stop, chosen formats/devices and every failed HRESULT through
`Log`/`LogIfFailed` (not on audio hot paths). The log is the only diagnostic a remote tester
can send, so log enough to reconstruct what happened.

## Behaviour carried over from macOS (keep identical)

**Devices** — video: MF VIDCAP sources. Virtual cameras (OBS Virtual Camera etc.) are
listed but never auto-selected: treat as virtual when the friendly name contains
"Virtual"/"OBS" or the symbolic link has no "vid_" USB id and is not a PCI device.
`looksLikeCaptureCard` = any native media type with width >= 1280 and fps >= 50.
Pairing order: container id, then USB VID:PID, then name.

**Auto selection (App)** — use the remembered device (symbolic link, then model "VID:PID",
then name). If one is remembered but missing: show
"“<name>”을(를) 찾을 수 없습니다.\nUSB 연결을 확인하거나, 오른쪽 클릭 › 비디오에서 다른 장치를 선택해 주세요."
and never fall back to another camera (a webcam's microphone would play on the speakers).
First run only: the first non-virtual device that looks like a capture card, preferring one
with a paired audio input. Remember the device after every successful start.

**Format choice (VideoCapture)** — automatic = among native types whose max rate reaches
min(requested or 50, best rate) − 0.5, the largest area; then the closest rate (or fastest);
then subtype NV12 > YUY2 > MJPG (MJPG is decoded to NV12 by the source reader). An exact
requested size wins when available. Never round the frame rate when applying: use the native
type's own MF_MT_FRAME_RATE. Menu values are rates rounded to 0.01 (59.94, 60).

**Video pipeline** — `MF_SOURCE_READER_ASYNC_CALLBACK` + `MF_LOW_LATENCY`; read the next sample
immediately after each one arrives; the renderer keeps only the newest frame and presents
right away (flip-model `DXGI_SWAP_EFFECT_FLIP_DISCARD`, 2 buffers, frame-latency waitable,
`SetMaximumFrameLatency(1)`, `Present(0, 0)`). YUV → RGB is BT.709 limited range. Letterbox
with black bars. Two passes: decode (NV12 or YUY2) to an RGBA texture at source size with
point sampling, then draw that with linear filtering + picture adjustments into the
letterboxed viewport. The same second pass renders snapshots at source size.

**Picture adjustments** — on gamma-encoded RGB in [0,1], in this order:
expandRange: `c = saturate((c - 16/255) * 255/219)`; gamma: `c = pow(c, 1/gamma)`;
contrast/brightness: `c = (c - 0.5) * contrast + 0.5 + brightness`;
saturation: `c = lerp(luma709(c), c, saturation)`; hue: Core Image CIHueAdjust, a luma-preserving
rotation (not Rodrigues): `M = A + cos(h)·B + sin(h)·C` with every row of A `(0.2125, 0.7154, 0.0721)`,
B rows `(0.7875, −0.7154, −0.0721) (−0.2125, 0.2846, −0.0721) (−0.2125, −0.7154, 0.9279)`,
C rows `(−0.2125, −0.7154, 0.9279) (0.1404, 0.1404, −0.2846) (−0.7875, 0.7154, 0.0721)`
(measured from CIHueAdjust); finally saturate.

**Audio** — capture endpoint in shared, event-driven mode with its mix format → float32
stereo (mono duplicated, extra channels dropped) → SPSC ring (power-of-two capacity, about
0.34 s) → render client on the default eConsole render endpoint, shared, event-driven,
initialised with float32 stereo at the capture rate plus `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`. A PI controller steers a linear-interpolation
resampler step (1 ± up to 1000 ppm) so the ring fill stays at target =
capture period + render period + 128 frames margin (+64 per jitter underrun, decays by 16 every
60 s only when the minimum headroom allowed it). Measure the fill extrapolated by the time
since the last capture write (no sawtooth). Underrun → silence and re-prime to target;
classify it as an *input gap* (don't raise the target) if the capture side reported
`AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` or stopped delivering, else as jitter. Gain = volume²
(0 when muted). Default output change → rebuild only the render side and reset the ring read
position. `AUDCLNT_E_DEVICE_INVALIDATED` → stop and post WM_APP_AUDIO_FAILED.
Latency estimate = capture period + fill + render padding + both `GetStreamLatency`.

**Picture dialog** — modeless tool window titled "화면 조정", owned by the main window,
rows: 밝기 (−30…+30, shown as "+12"/"0"), 대비 (50…150 %), 채도 (0…200 %), 감마 (0.50…1.50),
색조 (−180…180°); each with a value label and a "초기화" button; a checkbox
"색 범위 확장 (화면이 회색빛으로 뿌옇게 보일 때)" and a "모두 기본값" button. Snap to the default
when within 1.5 % of the range. Live updates while dragging. Malgun Gothic 9 pt, DPI aware.

**Clipboard** — PNG (via WIC, registered clipboard format "PNG") plus CF_DIB (BI_RGB,
bottom-up 32-bit). One OpenClipboard/EmptyClipboard/SetClipboardData sequence.

**UI (App)** — window title = device name (or "CapView"), black background, client area
locked to the video aspect while resizing (WM_SIZING). No menu bar: right-click menu
mirrors the Mac menus. Keys (IME disabled for the window with `ImmAssociateContextEx(hwnd,
nullptr, 0)` so Korean input mode doesn't swallow them): F / double-click = fullscreen
(borderless, covers the monitor), Esc leaves fullscreen, M mute, ↑/↓ volume ±10 %, Ctrl+C
screen capture, Ctrl+E 화면 조정, Ctrl+T always on top, Ctrl+0 actual pixel size, Ctrl+R
reconnect. Key repeats are ignored for toggles. Cursor hides after 2 s idle over the video.
`SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED)` while a picture is visible;
released after 5 minutes without frames. Watchdog every second: measured fps, "no signal"
message after 2 s without frames, restart the reader at 3/10/30 s and every 60 s; retry audio
every 5 s if it should be running. Toasts: "볼륨 70%", "음소거", "화면을 클립보드에 복사했습니다",
"캡처할 화면이 없습니다", "항상 위에 표시" / "항상 위에 표시 끔".
Camera privacy denied (E_ACCESSDENIED from MF) → message pointing to
설정 › 개인 정보 및 보안 › 카메라 › "데스크톱 앱이 카메라에 액세스하도록 허용", with a menu item that opens
`ms-settings:privacy-webcam` (and `ms-settings:privacy-microphone` for audio).
