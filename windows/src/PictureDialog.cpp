// The modeless "화면 조정" tool window: live sliders for the picture corrections.
// Built in code (no dialog template) so it needs no .rc and lays itself out per window DPI.

#include "PictureDialog.h"

#include <commctrl.h>
#include <imm.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cwchar>
#include <exception>
#include <type_traits>

namespace {

constexpr wchar_t kClassName[] = L"CapViewPictureDialog";
constexpr wchar_t kTitle[] = L"화면 조정";
constexpr wchar_t kRangeText[] = L"색 범위 확장 (화면이 회색빛으로 뿌옇게 보일 때)";
constexpr wchar_t kResetText[] = L"초기화";
constexpr wchar_t kResetAllText[] = L"모두 기본값";

constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
constexpr DWORD kExStyle = WS_EX_TOOLWINDOW;

// Control ids stay clear of IDOK / IDCANCEL, which IsDialogMessage sends for Enter / Esc.
constexpr int kIdStatic = -1;
constexpr int kIdSlider = 100;  // + row
constexpr int kIdReset = 120;   // + row
constexpr int kIdRange = 140;
constexpr int kIdResetAll = 141;

// Value texts exactly as the macOS panel shows them.
std::wstring FormatSigned(double value) {
    const long percent = std::lround(value * 100);
    return (percent > 0 ? L"+" : L"") + std::to_wstring(percent);
}

std::wstring FormatPercent(double value) {
    return std::to_wstring(std::lround(value * 100)) + L"%";
}

std::wstring FormatGamma(double value) {
    wchar_t text[32] = L"";
    std::swprintf(text, sizeof(text) / sizeof(text[0]), L"%.2f", value);
    return text;
}

std::wstring FormatDegrees(double value) {
    return std::to_wstring(std::lround(value)) + L"°";
}

struct RowSpec {
    const wchar_t* title;
    const wchar_t* key;  // for the log
    double PictureSettings::*field;
    double minValue;
    double maxValue;
    int scale;     // trackbar units per 1.0: trackbars only take integer positions
    int lineSize;  // arrow keys / wheel, in trackbar units (one displayed step)
    int pageSize;  // PgUp / PgDn and clicks on the channel
    std::wstring (*format)(double);
};

constexpr int kRowCount = 5;
const RowSpec kRows[kRowCount] = {
    {L"밝기", L"brightness", &PictureSettings::brightness, -0.3, 0.3, 1000, 10, 50, FormatSigned},
    {L"대비", L"contrast", &PictureSettings::contrast, 0.5, 1.5, 1000, 10, 100, FormatPercent},
    {L"채도", L"saturation", &PictureSettings::saturation, 0.0, 2.0, 1000, 10, 100, FormatPercent},
    {L"감마", L"gamma", &PictureSettings::gamma, 0.5, 1.5, 1000, 10, 100, FormatGamma},
    {L"색조", L"hue", &PictureSettings::hue, -180.0, 180.0, 10, 10, 150, FormatDegrees},
};

double DefaultOf(const RowSpec& row) {
    static const PictureSettings neutral{};
    return neutral.*row.field;
}

double ClampToRow(const RowSpec& row, double value) {
    if (!std::isfinite(value)) return DefaultOf(row);
    return std::clamp(value, row.minValue, row.maxValue);
}

// Snap to the default near the middle so "unchanged" is easy to hit (same rule as macOS).
double Snap(const RowSpec& row, double value) {
    const double neutral = DefaultOf(row);
    return std::fabs(value - neutral) < (row.maxValue - row.minValue) * 0.015 ? neutral : value;
}

int MinPos(const RowSpec& row) { return static_cast<int>(std::lround(row.minValue * row.scale)); }
int MaxPos(const RowSpec& row) { return static_cast<int>(std::lround(row.maxValue * row.scale)); }

int PosFor(const RowSpec& row, double value) {
    const int pos = static_cast<int>(std::lround(ClampToRow(row, value) * row.scale));
    return std::clamp(pos, MinPos(row), MaxPos(row));
}

double ValueFor(const RowSpec& row, int pos) {
    return ClampToRow(row, static_cast<double>(pos) / row.scale);
}

PictureSettings Sanitized(const PictureSettings& in) {
    PictureSettings out = in;
    for (const RowSpec& row : kRows) out.*row.field = ClampToRow(row, out.*row.field);
    return out;
}

// Keys the dialog's own controls use. Everything else typed while the dialog has focus goes
// to the main window, so its shortcuts (F, M, Ctrl+C, Ctrl+T ...) keep working.
bool IsDialogKey(WPARAM vk) {
    switch (vk) {
    case VK_TAB: case VK_RETURN: case VK_SPACE: case VK_ESCAPE:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LWIN: case VK_RWIN:
    case VK_PROCESSKEY:
        return true;
    default:
        return false;
    }
}

UINT DpiOf(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    if (dpi == 0) {
        if (HDC dc = GetDC(nullptr)) {
            dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY));
            ReleaseDC(nullptr, dc);
        }
    }
    return dpi ? dpi : 96;
}

struct FontDeleter {
    void operator()(HFONT font) const {
        // Deleting the stock fallback font is a harmless no-op.
        if (font) DeleteObject(font);
    }
};
using FontPtr = std::unique_ptr<std::remove_pointer_t<HFONT>, FontDeleter>;

FontPtr CreateUiFont(UINT dpi) {
    HFONT font = CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
    if (!font) {
        Log(L"PictureDialog: CreateFontW(Malgun Gothic, dpi %u) failed (%lu); using DEFAULT_GUI_FONT", dpi,
            GetLastError());
        font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    return FontPtr(font);
}

/// Pixel sizes for one DPI. Fixed sizes are in 96-DPI units scaled by S(); text-dependent
/// widths are measured with the real font so Korean glyphs and DPI rounding always fit.
struct Metrics {
    UINT dpi = 96;
    int margin = 0, colGap = 0, rowGap = 0, rowH = 0, trackH = 0, thumbLen = 0;
    int buttonH = 0, checkH = 0, gapAfterRows = 0, gapAfterCheck = 0;
    int labelW = 0, valueW = 0, resetW = 0, checkW = 0, resetAllW = 0, sliderMinW = 0;

    int S(int dip) const { return MulDiv(dip, static_cast<int>(dpi), 96); }

    int FixedRowWidth() const { return 2 * margin + labelW + valueW + resetW + 3 * colGap; }

    SIZE Client() const {
        const int width = std::max({FixedRowWidth() + sliderMinW, 2 * margin + checkW, 2 * margin + resetAllW});
        const int height = margin + kRowCount * rowH + (kRowCount - 1) * rowGap + gapAfterRows + checkH +
                           gapAfterCheck + buttonH + margin;
        return {width, height};
    }

    SIZE Window() const {
        const SIZE client = Client();
        RECT rect{0, 0, client.cx, client.cy};
        if (!AdjustWindowRectExForDpi(&rect, kStyle, FALSE, kExStyle, dpi)) {
            LogIfFailed(HRESULT_FROM_WIN32(GetLastError()), L"PictureDialog: AdjustWindowRectExForDpi");
            AdjustWindowRectEx(&rect, kStyle, FALSE, kExStyle);
        }
        return {rect.right - rect.left, rect.bottom - rect.top};
    }
};

Metrics Measure(UINT dpi, HFONT font) {
    Metrics m;
    m.dpi = dpi;

    HDC dc = GetDC(nullptr);
    HGDIOBJ previous = (dc && font) ? SelectObject(dc, font) : nullptr;
    auto textWidth = [&](const std::wstring& text) {
        SIZE size{};
        if (dc && GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size)) return int(size.cx);
        return static_cast<int>(text.size()) * m.S(12);  // rough fallback, errs on the wide side
    };
    TEXTMETRICW tm{};
    const int textH = (dc && GetTextMetricsW(dc, &tm)) ? int(tm.tmHeight) : m.S(16);

    for (const RowSpec& row : kRows) {
        m.labelW = std::max(m.labelW, textWidth(row.title));
        for (double sample : {row.minValue, row.maxValue, DefaultOf(row)})
            m.valueW = std::max(m.valueW, textWidth(row.format(sample)));
    }
    const int resetTextW = textWidth(kResetText);
    const int resetAllTextW = textWidth(kResetAllText);
    const int rangeTextW = textWidth(kRangeText);

    if (dc) {
        if (previous) SelectObject(dc, previous);
        ReleaseDC(nullptr, dc);
    }

    m.margin = m.S(12);
    m.colGap = m.S(8);
    m.rowGap = m.S(4);
    m.gapAfterRows = m.S(12);
    m.gapAfterCheck = m.S(14);
    // Fixed-length thumb sized for the DPI; the control is only a little taller than the thumb
    // so it looks centred in the row whatever offset the trackbar uses internally.
    m.thumbLen = m.S(20);
    m.trackH = m.thumbLen + m.S(4);
    m.buttonH = std::max(m.S(23), textH + m.S(7));
    m.rowH = std::max({m.S(26), m.trackH, m.buttonH, textH});
    m.checkH = std::max(m.S(20), textH + m.S(4));
    m.valueW += m.S(4);
    m.resetW = std::max(resetTextW + m.S(20), m.S(48));
    m.resetAllW = std::max(resetAllTextW + m.S(24), m.S(88));
    m.checkW = rangeTextW + GetSystemMetricsForDpi(SM_CXMENUCHECK, dpi) + m.S(8);
    m.sliderMinW = m.S(200);
    return m;
}

void InitCommonControlsOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;  // trackbar + tooltips, buttons/statics
    if (!InitCommonControlsEx(&icc)) Log(L"PictureDialog: InitCommonControlsEx failed");
}

}  // namespace

struct PictureDialog::Impl {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND titles[kRowCount]{};
    HWND sliders[kRowCount]{};
    HWND values[kRowCount]{};
    HWND resets[kRowCount]{};
    HWND range = nullptr;
    HWND resetAll = nullptr;
    HWND tooltip = nullptr;
    std::wstring valueTexts[kRowCount];

    FontPtr font;
    Metrics metrics;
    PictureSettings settings;
    ChangeHandler onChange;
    HWND lastFocus = nullptr;
    bool refreshing = false;            // our own TBM_SETPOS / BM_SETCHECK must not count as user input
    bool changedInGesture[kRowCount]{};  // log once per drag / key press, at TB_ENDTRACK

    ~Impl() {
        if (hwnd) DestroyWindow(hwnd);  // WM_NCDESTROY clears hwnd and the child handles
    }

    // MARK: Public operations

    void Show(HWND newOwner, const PictureSettings& current, ChangeHandler handler) {
        if (hwnd && newOwner != owner) {
            Log(L"PictureDialog: owner changed, recreating the window");
            DestroyWindow(hwnd);
        }
        owner = newOwner;
        settings = Sanitized(current);
        onChange = std::move(handler);

        if (!hwnd && !Create()) return;
        Refresh();

        if (IsWindowVisible(hwnd)) {
            SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetActiveWindow(hwnd);
            return;
        }
        // The position is kept between shows; only fix it if its monitor went away.
        if (!MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL)) {
            const POINT pos = InitialPosition(metrics.Window(), metrics.dpi);
            SetWindowPos(hwnd, nullptr, pos.x, pos.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            Log(L"PictureDialog: was off-screen, moved to %ld,%ld", pos.x, pos.y);
        }
        ShowWindow(hwnd, SW_SHOW);
        Log(L"PictureDialog: shown");
    }

    void Hide() {
        if (!hwnd || !IsWindowVisible(hwnd)) return;
        // Hand activation back to the player explicitly; otherwise Windows may pick another app.
        if (GetActiveWindow() == hwnd && owner && IsWindow(owner) && IsWindowVisible(owner) &&
            IsWindowEnabled(owner)) {
            SetActiveWindow(owner);
        }
        ShowWindow(hwnd, SW_HIDE);
        Log(L"PictureDialog: hidden");
    }

    bool PreTranslate(MSG* msg) {
        if (!msg || !hwnd || !msg->hwnd || !IsWindowVisible(hwnd)) return false;
        if (msg->hwnd != hwnd && !IsChild(hwnd, msg->hwnd)) return false;

        if (msg->message == WM_KEYDOWN || msg->message == WM_KEYUP) {
            if (msg->wParam == VK_ESCAPE) {
                if (msg->message == WM_KEYDOWN) Hide();
                return true;
            }
            if (!IsDialogKey(msg->wParam) && owner && IsWindow(owner)) {
                // Re-post unchanged (lParam keeps the repeat bit the player checks for toggles).
                // Posted, not sent, so the player handles it from its own message loop turn.
                PostMessageW(owner, msg->message, msg->wParam, msg->lParam);
                return true;
            }
        }
        return IsDialogMessageW(hwnd, msg) != FALSE;
    }

    // MARK: Window creation and layout

    static bool RegisterWindowClass(HINSTANCE instance) {
        static bool registered = false;
        if (registered) return true;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Impl::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // Dialog face colour; statics, checkbox and trackbars paint on it via WM_CTLCOLORSTATIC.
        wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_3DFACE + 1));
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Log(L"PictureDialog: RegisterClassExW failed (%lu)", GetLastError());
            return false;
        }
        registered = true;
        return true;
    }

    bool Create() {
        InitCommonControlsOnce();
        HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!RegisterWindowClass(instance)) return false;

        // Create directly at the final spot, sized for the owner's DPI: a per-monitor-aware
        // window takes the DPI of the monitor it is created on.
        const UINT guessDpi = DpiOf(owner);
        FontPtr newFont = CreateUiFont(guessDpi);
        Metrics m = Measure(guessDpi, newFont.get());
        SIZE size = m.Window();
        POINT pos = InitialPosition(size, guessDpi);

        font = std::move(newFont);
        metrics = m;
        HWND created = CreateWindowExW(kExStyle, kClassName, kTitle, kStyle, pos.x, pos.y, size.cx, size.cy, owner,
                                       nullptr, instance, this);
        if (!created) {
            Log(L"PictureDialog: CreateWindowExW failed (%lu)", GetLastError());
            hwnd = nullptr;
            return false;
        }
        hwnd = created;
        CreateControls(instance);
        ApplyFont();

        // Korean input mode would turn letters into VK_PROCESSKEY, which the player's shortcuts
        // (forwarded from here) don't understand. Nothing in this window takes text.
        if (!ImmAssociateContextEx(hwnd, nullptr, IACE_CHILDREN))
            Log(L"PictureDialog: ImmAssociateContextEx failed (%lu)", GetLastError());

        const UINT actualDpi = DpiOf(hwnd);
        if (actualDpi != guessDpi) {
            Log(L"PictureDialog: created at dpi %u, owner dpi %u", actualDpi, guessDpi);
            ApplyDpi(actualDpi);
            size = metrics.Window();
            pos = InitialPosition(size, actualDpi);
            SetWindowPos(hwnd, nullptr, pos.x, pos.y, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        Layout();
        Log(L"PictureDialog: created at %ld,%ld, %ldx%ld px, dpi %u", pos.x, pos.y, size.cx, size.cy, metrics.dpi);
        return true;
    }

    /// Near the top-right corner inside the owner's client area (macOS: 16 pt from the right,
    /// just under the title bar), kept within that monitor's work area.
    POINT InitialPosition(SIZE size, UINT dpi) const {
        auto S = [dpi](int dip) { return MulDiv(dip, static_cast<int>(dpi), 96); };
        RECT area{};
        bool haveOwner = false;
        if (owner && IsWindow(owner) && IsWindowVisible(owner) && !IsIconic(owner)) {
            RECT client{};
            if (GetClientRect(owner, &client) && client.right > client.left && client.bottom > client.top) {
                MapWindowPoints(owner, nullptr, reinterpret_cast<POINT*>(&client), 2);
                area = client;
                haveOwner = true;
            }
        }
        HMONITOR monitor = haveOwner ? MonitorFromRect(&area, MONITOR_DEFAULTTONEAREST)
                                     : MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY);
        RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (monitor && GetMonitorInfoW(monitor, &info)) work = info.rcWork;
        if (!haveOwner) area = work;

        LONG x = area.right - size.cx - S(16);
        LONG y = area.top + S(12);
        x = std::max(work.left, std::min(x, work.right - size.cx));
        y = std::max(work.top, std::min(y, work.bottom - size.cy));
        return {x, y};
    }

    HWND MakeChild(HINSTANCE instance, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (!child) Log(L"PictureDialog: creating a %ls control failed (%lu)", cls, GetLastError());
        return child;
    }

    void CreateControls(HINSTANCE instance) {
        // Creation order is the tab order and puts each label right before its slider, which
        // screen readers use as the slider's name. Every control is its own WS_GROUP so the
        // arrow keys only ever move a slider, never the focus.
        constexpr DWORD kLabel = SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX | WS_GROUP;
        for (int i = 0; i < kRowCount; ++i) {
            const RowSpec& row = kRows[i];
            titles[i] = MakeChild(instance, L"STATIC", row.title, kLabel, kIdStatic);
            sliders[i] = MakeChild(instance, TRACKBAR_CLASSW, row.title,
                                   TBS_HORZ | TBS_BOTH | TBS_NOTICKS | TBS_FIXEDLENGTH | WS_TABSTOP | WS_GROUP,
                                   kIdSlider + i);
            if (HWND slider = sliders[i]) {
                SendMessageW(slider, TBM_SETRANGEMIN, FALSE, MinPos(row));
                SendMessageW(slider, TBM_SETRANGEMAX, FALSE, MaxPos(row));
                SendMessageW(slider, TBM_SETLINESIZE, 0, row.lineSize);
                SendMessageW(slider, TBM_SETPAGESIZE, 0, row.pageSize);
            }
            values[i] = MakeChild(instance, L"STATIC", L"", kLabel, kIdStatic);
            resets[i] = MakeChild(instance, L"BUTTON", kResetText, BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, kIdReset + i);
        }
        range = MakeChild(instance, L"BUTTON", kRangeText, BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, kIdRange);
        resetAll = MakeChild(instance, L"BUTTON", kResetAllText, BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, kIdResetAll);

        // "밝기 초기화" etc.: five identical "초기화" buttons need to say which row they reset.
        tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, instance,
                                  nullptr);
        if (!tooltip) {
            Log(L"PictureDialog: creating the tooltip failed (%lu)", GetLastError());
            return;
        }
        for (int i = 0; i < kRowCount; ++i) {
            if (!resets[i]) continue;
            std::wstring tip = std::wstring(kRows[i].title) + L" " + kResetText;
            TTTOOLINFOW info{};
            // The pre-v6 size works with both comctl32 5 and 6 (v6 accepts the smaller struct).
            info.cbSize = static_cast<UINT>(offsetof(TTTOOLINFOW, lParam) + sizeof(info.lParam));
            info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            info.hwnd = hwnd;
            info.uId = reinterpret_cast<UINT_PTR>(resets[i]);
            info.lpszText = tip.data();  // copied by the tooltip
            if (!SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info)))
                Log(L"PictureDialog: TTM_ADDTOOL failed for row %d", i);
        }
    }

    void ForEachChild(const std::function<void(HWND)>& body) const {
        for (int i = 0; i < kRowCount; ++i) {
            for (HWND child : {titles[i], sliders[i], values[i], resets[i]})
                if (child) body(child);
        }
        if (range) body(range);
        if (resetAll) body(resetAll);
    }

    void ApplyFont() {
        HFONT current = font.get();
        ForEachChild([current](HWND child) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(current), TRUE);
        });
        for (HWND slider : sliders)
            if (slider) SendMessageW(slider, TBM_SETTHUMBLENGTH, static_cast<WPARAM>(metrics.thumbLen), 0);
    }

    void ApplyDpi(UINT dpi) {
        FontPtr newFont = CreateUiFont(dpi);
        metrics = Measure(dpi, newFont.get());
        FontPtr oldFont = std::move(font);
        font = std::move(newFont);
        ApplyFont();  // children stop using the old font before it is deleted at scope exit
    }

    /// Fixed columns; the slider takes whatever width is left, which also absorbs rounding
    /// differences when Windows sizes the window for a new DPI.
    void Layout() {
        if (!hwnd || !sliders[0]) return;  // WM_SIZE during CreateWindowEx, before the children exist
        RECT client{};
        GetClientRect(hwnd, &client);
        const Metrics& m = metrics;
        const int width = client.right - client.left;
        const int sliderW = std::max(width - m.FixedRowWidth(), m.S(80));

        auto place = [](HWND child, int x, int y, int w, int h) {
            if (child) SetWindowPos(child, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        };
        int y = m.margin;
        for (int i = 0; i < kRowCount; ++i) {
            int x = m.margin;
            place(titles[i], x, y, m.labelW, m.rowH);
            x += m.labelW + m.colGap;
            place(sliders[i], x, y + (m.rowH - m.trackH) / 2, sliderW, m.trackH);
            x += sliderW + m.colGap;
            place(values[i], x, y, m.valueW, m.rowH);
            x += m.valueW + m.colGap;
            place(resets[i], x, y + (m.rowH - m.buttonH) / 2, m.resetW, m.buttonH);
            y += m.rowH + m.rowGap;
        }
        y += m.gapAfterRows - m.rowGap;
        place(range, m.margin, y, std::min(m.checkW, std::max(width - 2 * m.margin, 0)), m.checkH);
        y += m.checkH + m.gapAfterCheck;
        place(resetAll, m.margin, y, m.resetAllW, m.buttonH);
    }

    // MARK: Values

    void SetValueText(int row) {
        std::wstring text = kRows[row].format(settings.*kRows[row].field);
        if (text == valueTexts[row]) return;  // avoid repainting the label on every mouse move
        valueTexts[row] = std::move(text);
        if (values[row]) SetWindowTextW(values[row], valueTexts[row].c_str());
    }

    void Refresh() {
        refreshing = true;
        for (int i = 0; i < kRowCount; ++i) {
            if (sliders[i]) SendMessageW(sliders[i], TBM_SETPOS, TRUE, PosFor(kRows[i], settings.*kRows[i].field));
            SetValueText(i);
            changedInGesture[i] = false;
        }
        if (range) SendMessageW(range, BM_SETCHECK, settings.expandRange ? BST_CHECKED : BST_UNCHECKED, 0);
        refreshing = false;
    }

    void Notify() {
        // Copies: the handler may call Show(), which replaces both onChange and settings.
        const ChangeHandler handler = onChange;
        const PictureSettings snapshot = settings;
        if (handler) handler(snapshot);
    }

    void OnScroll(HWND slider, int code) {
        if (refreshing) return;
        int row = -1;
        for (int i = 0; i < kRowCount; ++i)
            if (sliders[i] == slider) row = i;
        if (row < 0) return;
        const RowSpec& spec = kRows[row];

        if (code == TB_ENDTRACK) {  // end of a drag / key press / channel click: no new position
            if (changedInGesture[row]) {
                changedInGesture[row] = false;
                Log(L"PictureDialog: %ls = %.3f", spec.key, settings.*spec.field);
            }
            return;
        }
        // TBM_GETPOS rather than HIWORD(wParam): that is 16-bit, unsigned, and only set for
        // the thumb codes. Like the macOS slider, the thumb stays where the user put it; only
        // the value snaps.
        const int pos = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        const double value = Snap(spec, ValueFor(spec, pos));
        double& field = settings.*spec.field;
        if (value == field) return;
        field = value;
        SetValueText(row);
        changedInGesture[row] = true;
        Notify();
    }

    void ResetRow(int row) {
        const RowSpec& spec = kRows[row];
        settings.*spec.field = DefaultOf(spec);
        Refresh();
        Log(L"PictureDialog: %ls reset", spec.key);
        Notify();
    }

    void ResetAll() {
        settings = PictureSettings{};
        Refresh();
        Log(L"PictureDialog: all reset");
        Notify();
    }

    void OnRangeToggled() {
        if (refreshing || !range) return;
        settings.expandRange = SendMessageW(range, BM_GETCHECK, 0, 0) == BST_CHECKED;
        Log(L"PictureDialog: expandRange = %d", settings.expandRange ? 1 : 0);
        Notify();
    }

    /// Enter: IsDialogMessage sends IDOK (there is no default button), so press the focused
    /// push button like a real dialog would.
    void ClickFocusedButton() {
        HWND focus = GetFocus();
        if (!focus) return;
        for (HWND button : resets) {
            if (button && button == focus) {
                SendMessageW(button, BM_CLICK, 0, 0);
                return;
            }
        }
        if (resetAll && focus == resetAll) SendMessageW(resetAll, BM_CLICK, 0, 0);
    }

    HWND FocusTarget() const {
        if (lastFocus && IsWindow(lastFocus) && IsChild(hwnd, lastFocus)) return lastFocus;
        return sliders[0];
    }

    // MARK: Messages

    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_HSCROLL:
            if (lParam) OnScroll(reinterpret_cast<HWND>(lParam), LOWORD(wParam));
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == IDCANCEL) {
                Hide();
                return 0;
            }
            if (id == IDOK) {
                ClickFocusedButton();
                return 0;
            }
            if (HIWORD(wParam) == BN_CLICKED) {
                if (id >= kIdReset && id < kIdReset + kRowCount) ResetRow(id - kIdReset);
                else if (id == kIdRange) OnRangeToggled();
                else if (id == kIdResetAll) ResetAll();
            }
            return 0;
        }

        case WM_ACTIVATE:
            // Plain windows don't remember the focused control like dialogs do; keep it so
            // Tab / arrows continue where they left off.
            if (LOWORD(wParam) == WA_INACTIVE) {
                HWND focus = GetFocus();
                if (focus && IsChild(window, focus)) lastFocus = focus;
                return 0;
            }
            if (HWND target = FocusTarget()) {
                SetFocus(target);
                return 0;  // DefWindowProc would focus the frame itself
            }
            break;

        case WM_SETFOCUS:
            if (HWND target = FocusTarget()) SetFocus(target);
            return 0;

        case WM_CLOSE:
            Hide();  // keep the window and its state; the destructor destroys it
            return 0;

        case WM_SIZE:
            Layout();
            return 0;

        case WM_GETDPISCALEDSIZE: {
            // Tell Windows our real size at the new DPI so the WM_DPICHANGED rectangle fits it
            // exactly (prevents bouncing between monitors near the edge).
            auto* size = reinterpret_cast<SIZE*>(lParam);
            const UINT dpi = static_cast<UINT>(wParam);
            if (!size || dpi == 0) return FALSE;
            FontPtr probe = CreateUiFont(dpi);
            *size = Measure(dpi, probe.get()).Window();
            return TRUE;
        }

        case WM_DPICHANGED: {
            const UINT dpi = LOWORD(wParam);
            ApplyDpi(dpi ? dpi : 96);
            if (const auto* suggested = reinterpret_cast<const RECT*>(lParam)) {
                SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                             suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            Layout();
            Log(L"PictureDialog: DPI changed to %u", metrics.dpi);
            return 0;
        }

        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            hwnd = nullptr;
            range = resetAll = tooltip = lastFocus = nullptr;
            for (int i = 0; i < kRowCount; ++i) {
                titles[i] = sliders[i] = values[i] = resets[i] = nullptr;
                valueTexts[i].clear();
                changedInGesture[i] = false;
            }
            Log(L"PictureDialog: window destroyed");
            break;

        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        Impl* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->hwnd = window;  // WM_CREATE / WM_SIZE arrive before CreateWindowExW returns
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);

        // Exceptions (e.g. from the change handler) must not unwind through user32.
        try {
            return self->HandleMessage(window, message, wParam, lParam);
        } catch (const std::exception& e) {
            wchar_t what[256] = L"";
            MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, what, 256);
            what[255] = L'\0';
            Log(L"PictureDialog: exception while handling message 0x%04X: %ls", message, what);
        } catch (...) {
            Log(L"PictureDialog: unknown exception while handling message 0x%04X", message);
        }
        return 0;
    }
};

PictureDialog::PictureDialog() : impl_(std::make_unique<Impl>()) {}

PictureDialog::~PictureDialog() = default;

void PictureDialog::Show(HWND owner, const PictureSettings& current, ChangeHandler onChange) {
    impl_->Show(owner, current, std::move(onChange));
}

void PictureDialog::Close() {
    impl_->Hide();
}

bool PictureDialog::IsOpen() const {
    return impl_->hwnd && IsWindowVisible(impl_->hwnd);
}

bool PictureDialog::PreTranslate(MSG* msg) {
    return impl_->PreTranslate(msg);
}
