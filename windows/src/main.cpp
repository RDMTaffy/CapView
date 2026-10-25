// CapView for Windows: entry point.
#include "App.h"

#include <mfapi.h>
#include <objbase.h>

namespace {

constexpr wchar_t kMediaFeaturePackMessage[] =
    L"Windows 미디어 기능을 시작하지 못했습니다.\n"
    L"Windows N/KN 에디션이라면 '미디어 기능 팩'을 설치해 주세요.\n"
    L"(설정 › 앱 › 선택적 기능 › 기능 추가 › 미디어 기능 팩)";

/// The Media Foundation DLLs are delay-loaded (build.sh), so on Windows N/KN without the Media
/// Feature Pack the app gets this far and can explain, instead of the loader's "MFPlat.DLL was
/// not found". Every MF call comes after this check.
bool MediaFoundationPresent() {
    for (const wchar_t* dll : {L"mfplat.dll", L"mf.dll", L"mfreadwrite.dll"}) {
        HMODULE module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            Log(L"%ls could not be loaded (error %lu); Windows N/KN without the Media Feature Pack?", dll,
                static_cast<unsigned long>(GetLastError()));
            return false;
        }
        // Kept loaded: the delay-load helper finds it again.
    }
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    // The manifest also declares Per-Monitor V2; this covers launches without it.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (!MediaFoundationPresent()) {
        MessageBoxW(nullptr, kMediaFeaturePackMessage, L"CapView", MB_ICONERROR);
        return 1;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return 1;
    }
    if (FAILED(LogIfFailed(MFStartup(MF_VERSION), L"MFStartup"))) {
        MessageBoxW(nullptr, kMediaFeaturePackMessage, L"CapView", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    Log(L"CapView starting");

    int result = 0;
    {
        App app;
        result = app.Run(instance, showCommand);
    }

    if (AnyThreadAbandoned()) {
        // A worker is still inside a driver call. Shutting MF/COM down or a normal exit (which
        // runs every DLL's detach code) could block on that driver again; settings are saved.
        Log(L"CapView exited (%d), ending the process: a worker is stuck in a driver", result);
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(result));
    }
    MFShutdown();
    CoUninitialize();
    Log(L"CapView exited (%d)", result);
    return result;
}
