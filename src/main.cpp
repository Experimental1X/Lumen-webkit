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

#include <atomic>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

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

// ---------------------------------------------------------------------------
// Freeze diagnostics
// ---------------------------------------------------------------------------
//
// The window has been seen to stop answering clicks entirely - tab switching
// dead, close button dead, page still faintly repainting underneath - while
// the machine stayed alive. Nothing in the log said where the UI thread went,
// because "the UI is frozen" is a symptom, not a cause, and there are several
// very different ways to get here: stuck inside one handler, stuck in a wait
// that never returns, or spinning through a queue that never drains. Guessing
// has already cost a few build cycles.
//
// So the pump keeps a small ring of the messages it has run, marking each one
// as it starts and again as it finishes, and a watchdog thread watches that
// ring move. A one-hertz timer on the window gives the pump a message even
// when nothing else is happening, which is what distinguishes "idle and
// healthy" from "wedged". When the ring stops, the watchdog writes the last
// message that started and never finished: that is the handler the thread was
// sitting in, named outright.
//
// The watchdog writes to its own file rather than to lumen.log. lumen::log
// opens and closes the file through the CRT, and the UI thread may itself be
// stuck inside one of those calls - in which case reusing it would hang the
// watchdog too and lose the diagnosis it exists to capture.
constexpr UINT_PTR kPumpTimer = 0x4C01;
constexpr size_t kPumpRingSize = 64;
// Silence this long is a wedge: the timer fires every second, so a healthy
// pump cannot go a full five without being seen.
constexpr unsigned long long kPumpStaleMs = 5000;
// ...and if it is still stuck this much later the process is going nowhere,
// so end it rather than leave a dead window that has to be killed by hand.
constexpr unsigned long long kPumpExitMs = 15000;

struct PumpEntry {
    UINT message { 0 };
    WPARAM wParam { 0 };
    LPARAM lParam { 0 };
    unsigned long long stamp { 0 };
    bool completed { false };
};

// Written by the UI thread only.
PumpEntry g_ring[kPumpRingSize];
size_t g_ringNext { 0 };
// Read by the watchdog thread.
std::atomic<unsigned long long> g_lastBeat { 0 };
std::atomic<bool> g_pumpStopping { false };
std::atomic<bool> g_pumpReported { false };

void pumpRecord(UINT message, WPARAM wParam, LPARAM lParam, bool completed)
{
    PumpEntry& entry = g_ring[g_ringNext];
    g_ringNext = (g_ringNext + 1) % kPumpRingSize;
    entry.message = message;
    entry.wParam = wParam;
    entry.lParam = lParam;
    entry.stamp = GetTickCount64();
    entry.completed = completed;
    g_lastBeat.store(entry.stamp, std::memory_order_release);
}

// Plain Win32 append. Deliberately not lumen::log - see the note above.
void watchdogLog(const std::wstring& text)
{
    const std::wstring path = lumen::executableDirectory() + L"\\lumen-watchdog.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME now { };
    GetLocalTime(&now);
    wchar_t stamp[32] { };
    std::swprintf(stamp, std::size(stamp), L"[%02d:%02d:%02d] ",
        now.wHour, now.wMinute, now.wSecond);

    const std::wstring line = std::wstring(stamp) + text + L"\r\n";
    DWORD written = 0;
    WriteFile(file, line.c_str(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
}

void pumpWatchdog()
{
    while (!g_pumpStopping.load(std::memory_order_acquire)) {
        Sleep(500);

        const unsigned long long now = GetTickCount64();
        const unsigned long long beat = g_lastBeat.load(std::memory_order_acquire);
        if (beat == 0 || now - beat < kPumpStaleMs)
            continue;

        if (!g_pumpReported.exchange(true)) {
            // The culprit is the newest entry that started and never finished.
            // The array wraps, so walk it from the next free slot to keep the
            // answer "the last thing that began" whatever the wrap point is.
            const PumpEntry* stuck = nullptr;
            for (size_t step = 0; step < kPumpRingSize; ++step) {
                const PumpEntry& entry = g_ring[(g_ringNext + step) % kPumpRingSize];
                if (entry.stamp != 0 && !entry.completed)
                    stuck = &entry;
            }

            wchar_t line[320] { };
            if (stuck) {
                std::swprintf(line, std::size(line),
                    L"UI THREAD STALLED for %llu ms. Stuck inside: msg=0x%04X wParam=0x%llX lParam=0x%llX.",
                    now - beat,
                    static_cast<unsigned>(stuck->message),
                    static_cast<unsigned long long>(stuck->wParam),
                    static_cast<unsigned long long>(stuck->lParam));
            } else {
                std::swprintf(line, std::size(line),
                    L"UI THREAD STALLED for %llu ms between messages - the pump is not being reached, so this is a spin or a wait, not a handler.",
                    now - beat);
            }
            watchdogLog(line);

            // Dump the recent history too. A handler can complete but leave
            // something behind that only bites on the next pass.
            wchar_t tail[1024] { };
            std::size_t used = 0;
            for (size_t step = 0; step < kPumpRingSize && used < 900u; ++step) {
                const PumpEntry& entry = g_ring[(g_ringNext + step) % kPumpRingSize];
                if (entry.stamp == 0)
                    continue;
                const int written = std::swprintf(tail + used, std::size(tail) - used,
                    L"  %6llu ms ago  msg=0x%04X wParam=0x%llX %ls\r\n",
                    now - entry.stamp,
                    static_cast<unsigned>(entry.message),
                    static_cast<unsigned long long>(entry.wParam),
                    entry.completed ? L"done" : L"STUCK");
                // A negative return means the line did not fit or failed to
                // encode; either way there is nothing useful left to add, and
                // adding it would run the index off the end of the buffer.
                if (written <= 0)
                    break;
                used += static_cast<std::size_t>(written);
                if (used >= std::size(tail))
                    used = std::size(tail) - 1;
            }
            watchdogLog(std::wstring(L"Recent messages, oldest first:") + L"\r\n" + tail);
        }

        if (now - beat >= kPumpExitMs) {
            watchdogLog(L"UI thread never came back; ending the process so the window is not left dead.");
            // There is no clean shutdown available: the thread that owns the
            // WebKit objects is the one stuck, and TerminateProcess is the
            // only way out that still gives us a usable desktop.
            TerminateProcess(GetCurrentProcess(), 2);
            return;
        }
    }
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

    case WM_NCCALCSIZE: {
        if (!wParam)
            break;

        // Frameless. The window keeps WS_CAPTION | WS_THICKCAPTION in its style
        // on purpose: removing them would cost us Aero Snap, the Win11 snap
        // layout flyout and the maximise/restore animation. Instead we claim
        // the entire window rect as client area, so the system frame is never
        // painted and Browser::onPaint covers it edge to edge.
        auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
        const RECT original = params->rgrc[0];
        const LRESULT result = DefWindowProcW(window, message, wParam, lParam);

        if (IsZoomed(window)) {
            // A maximised frame sits just off-screen so its resize border is
            // invisible. Only the caption still needs to be handed back.
            const UINT dpi = GetDpiForWindow(window);
            const int frame = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            params->rgrc[0].top = original.top + frame;
        } else
            params->rgrc[0] = original;

        return result;
    }

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

    case WM_LBUTTONUP:
        // Releases the pressed state so the button springs back out.
        if (browser)
            browser->onLeftButtonUp();
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
        // The pump's own heartbeat. It exists only so that a healthy but idle
        // browser still produces a message once a second, which is what lets
        // the watchdog tell "nothing is happening" apart from "the thread is
        // stuck". Browser has timers of its own on the same window and must
        // not be handed this one.
        if (wParam == kPumpTimer)
            return 0;
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

    case WM_LUMEN_SETTINGS_CLEAR_HISTORY:
        if (browser)
            browser->onClearHistory();
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

    // Start the watchdog and the heartbeat before the pump, so a stall during
    // startup is caught too.
    lumen::log(L"Pump watchdog running: a wedged UI thread is written to lumen-watchdog.log after 5 s.");
    g_pumpStopping.store(false, std::memory_order_release);
    g_pumpReported.store(false, std::memory_order_release);
    SetTimer(window, kPumpTimer, 1000, nullptr);
    std::thread watchdog(pumpWatchdog);

    MSG message { };
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        pumpRecord(message.message, message.wParam, message.lParam, false);

        if (accelerators && TranslateAcceleratorW(window, accelerators, &message)) {
            pumpRecord(message.message, message.wParam, message.lParam, true);
            continue;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
        pumpRecord(message.message, message.wParam, message.lParam, true);
    }

    KillTimer(window, kPumpTimer);
    g_pumpStopping.store(true, std::memory_order_release);
    watchdog.join();

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
