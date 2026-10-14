// Lumen: a WebKit-based browser for Windows with a Safari-style interface.
//
// This file owns the main window and forwards messages to Browser (see
// browser.h), which draws the chrome and manages the tabs.

#include "browser.h"
#include "downloads.h"
#include "settings.h"
#include "util.h"

#include <windows.h>
#include <windowsx.h>

#include <cstdint>
#include <cwchar>
#include <iterator>
#include <memory>
#include <string>

namespace {

// Writes where a crash happened to lumen.log (module and offset), so a crash
// can be diagnosed from the log alone.
LONG WINAPI crashFilter(EXCEPTION_POINTERS* info)
{
    wchar_t module[MAX_PATH] = L"?";
    unsigned long long offset = 0;
    const void* address = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionAddress : nullptr;
    const unsigned long code = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionCode : 0;

    HMODULE handle = nullptr;
    if (address
        && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &handle)
        && handle) {
        GetModuleFileNameW(handle, module, static_cast<DWORD>(std::size(module)));
        offset = static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(handle));
    }

    wchar_t line[MAX_PATH + 160] { };
    std::swprintf(line, std::size(line), L"CRASH: exception 0x%08lX at %p in %ls +0x%llX", code, address, module, offset);
    lumen::log(line);
    return EXCEPTION_CONTINUE_SEARCH;
}

constexpr wchar_t kWindowClass[] = L"LumenMainWindow";
constexpr wchar_t kAppName[] = L"Lumen";

// WebKit starts helper processes from the same folder as the executable.
// Check up front so a missing file produces a readable message.
bool verifyRuntimeFiles(std::wstring& missing)
{
    static const wchar_t* const required[] = {
        L"WebKit2.dll",
        L"JavaScriptCore.dll",
        L"WebCore.dll",
        L"WebKitWebProcess.exe",
        L"WebKitNetworkProcess.exe",
    };

    const std::wstring directory = lumen::executableDirectory();
    for (const wchar_t* name : required) {
        const std::wstring path = directory + L"\\" + name;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            missing += std::wstring(L"  ") + name + L"\n";
    }
    return missing.empty();
}

HACCEL createAccelerators()
{
    using namespace lumen;

    ACCEL keys[] = {
        { FCONTROL | FVIRTKEY, 'T', kCmdNewTab },
        { FCONTROL | FVIRTKEY, 'W', kCmdCloseTab },
        { FCONTROL | FVIRTKEY, 'L', kCmdFocusAddress },
        { FCONTROL | FVIRTKEY, 'R', kCmdReload },
        { FVIRTKEY, VK_F5, kCmdReload },
        { FALT | FVIRTKEY, VK_LEFT, kCmdBack },
        { FALT | FVIRTKEY, VK_RIGHT, kCmdForward },
        { FCONTROL | FVIRTKEY, VK_OEM_4, kCmdBack },
        { FCONTROL | FVIRTKEY, VK_OEM_6, kCmdForward },
        { FCONTROL | FVIRTKEY, VK_OEM_PLUS, kCmdZoomIn },
        { FCONTROL | FVIRTKEY, VK_ADD, kCmdZoomIn },
        { FCONTROL | FVIRTKEY, VK_OEM_MINUS, kCmdZoomOut },
        { FCONTROL | FVIRTKEY, VK_SUBTRACT, kCmdZoomOut },
        { FCONTROL | FVIRTKEY, '0', kCmdZoomReset },
        { FCONTROL | FVIRTKEY, VK_TAB, kCmdNextTab },
        { FCONTROL | FSHIFT | FVIRTKEY, VK_TAB, kCmdPrevTab },
        { FCONTROL | FVIRTKEY, 'D', kCmdBookmarkPage },
        { FCONTROL | FSHIFT | FVIRTKEY, 'D', kCmdReadingList },
        { FCONTROL | FSHIFT | FVIRTKEY, 'L', kCmdSidebar },
        { FCONTROL | FSHIFT | FVIRTKEY, VK_OEM_5, kCmdTabOverview },
        { FCONTROL | FVIRTKEY, 'F', kCmdFind },
        { FCONTROL | FVIRTKEY, 'G', kCmdFindNext },
        { FCONTROL | FSHIFT | FVIRTKEY, 'G', kCmdFindPrevious },
        { FVIRTKEY, VK_F3, kCmdFindNext },
        { FSHIFT | FVIRTKEY, VK_F3, kCmdFindPrevious },
        { FCONTROL | FVIRTKEY, 'J', kCmdDownloads },
    };
    return CreateAcceleratorTableW(keys, static_cast<int>(sizeof(keys) / sizeof(keys[0])));
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wParam, lParam);
    }

    auto* browser = reinterpret_cast<lumen::Browser*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        std::wstring error;
        if (!browser->create(window, error)) {
            lumen::log(L"Startup failed: " + error);
            MessageBoxW(window, error.c_str(), kAppName, MB_OK | MB_ICONERROR);
            return -1;
        }
        return 0;
    }

    case WM_NCCALCSIZE:
        if (wParam) {
            // Remove the native title bar but keep the thin resize borders,
            // shadow and snapping: let the default procedure compute the
            // client rectangle, then give the caption height back to us.
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            const LONG originalTop = params->rgrc[0].top;
            const LRESULT result = DefWindowProcW(window, message, wParam, lParam);
            if (IsZoomed(window)) {
                const UINT dpi = GetDpiForWindow(window);
                const int frame = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                    + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                params->rgrc[0].top = originalTop + frame;
            } else
                params->rgrc[0].top = originalTop;
            return result;
        }
        break;

    case WM_NCHITTEST: {
        const LRESULT hit = DefWindowProcW(window, message, wParam, lParam);
        if (browser)
            return browser->onNcHitTest(hit, POINT { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return hit;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint { };
        HDC dc = BeginPaint(window, &paint);
        if (browser)
            browser->onPaint(dc);
        EndPaint(window, &paint);
        return 0;
    }

    case WM_SIZE:
        if (browser)
            browser->onSize();
        return 0;

    case WM_LBUTTONDOWN:
        if (browser && browser->onLeftButtonDown(POINT { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }))
            return 0;
        break;

    case WM_MOUSEMOVE:
        if (browser) {
            if (!browser->trackingMouse()) {
                TRACKMOUSEEVENT track { sizeof(track), TME_LEAVE, window, 0 };
                if (TrackMouseEvent(&track))
                    browser->setTrackingMouse(true);
            }
            browser->onMouseMove(POINT { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        }
        return 0;

    case WM_MOUSELEAVE:
        if (browser) {
            browser->setTrackingMouse(false);
            browser->onMouseLeave();
        }
        return 0;

    case WM_NCMOUSEMOVE:
        if (browser)
            browser->onNcMouseMove();
        break;

    case WM_MOUSEWHEEL:
        if (browser && browser->onMouseWheel(wParam, lParam))
            return 0;
        break;

    case WM_KEYDOWN:
        if (browser && browser->onKeyDown(wParam))
            return 0;
        break;

    case WM_TIMER:
        if (browser)
            browser->onTimer(wParam);
        return 0;

    case WM_SETFOCUS:
        if (browser)
            browser->onSetFocus();
        return 0;

    case WM_ACTIVATE:
        if (browser && LOWORD(wParam) != WA_INACTIVE)
            browser->onSetFocus();
        break;

    case WM_CTLCOLOREDIT:
        if (browser) {
            const LRESULT brush = browser->onCtlColorEdit(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam));
            if (brush)
                return brush;
        }
        break;

    case WM_SETTINGCHANGE:
        if (browser)
            browser->onSettingChange(lParam);
        break;

    case WM_COMMAND:
        // lParam is zero for menu and accelerator commands; notifications
        // from child controls (the address field) are ignored.
        if (browser && lParam == 0) {
            browser->onCommand(LOWORD(wParam));
            return 0;
        }
        if (browser && lParam != 0) {
            browser->onControlNotify(reinterpret_cast<HWND>(lParam), HIWORD(wParam));
            return 0;
        }
        break;

    case WM_LUMEN_DOWNLOAD_CHANGED:
        if (browser)
            browser->onDownloadsChanged(wParam != 0);
        return 0;

    case WM_LUMEN_SETTINGS_CHANGED:
        if (browser)
            browser->onSettingsChanged();
        return 0;

    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        if (browser)
            browser->onDpiChanged();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 780;
        info->ptMinTrackSize.y = 480;
        return 0;
    }

    case WM_DESTROY:
        if (browser)
            browser->destroy();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

int run(HINSTANCE instance, int showCommand)
{
    std::wstring missing;
    if (!verifyRuntimeFiles(missing)) {
        const std::wstring text = L"These WebKit files are missing next to Lumen.exe:\n\n" + missing
            + L"\nExtract the whole Lumen zip into one folder and run it from there.";
        lumen::log(text);
        MessageBoxW(nullptr, text.c_str(), kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW windowClass { };
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        MessageBoxW(nullptr, L"Could not register the window class.", kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }

    lumen::Browser browser;
    HWND window = CreateWindowExW(
        0,
        kWindowClass,
        kAppName,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 840,
        nullptr, nullptr, instance, &browser);
    if (!window)
        return 1;

    HACCEL accelerators = createAccelerators();

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message { };
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (accelerators && TranslateAcceleratorW(window, accelerators, &message))
            continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (accelerators)
        DestroyAcceleratorTable(accelerators);

    return static_cast<int>(message.wParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    SetUnhandledExceptionFilter(crashFilter);
    lumen::log(L"Starting Lumen.");

    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    const int exitCode = run(instance, showCommand);

    Gdiplus::GdiplusShutdown(gdiplusToken);
    return exitCode;
}
