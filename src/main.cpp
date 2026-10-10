// Lumen: a WebKit-based browser for Windows.
//
// This file is deliberately small. The window currently hosts a single web
// view with no custom chrome; the browser UI is added in later milestones
// (see docs/ROADMAP.md). Its job right now is to prove that the app links
// against the prebuilt WebKit SDK and that WebKit actually renders a page.

#include "util.h"
#include "webkit_host.h"

#include <windows.h>
#include <windowsx.h>

#include <memory>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"LumenMainWindow";
constexpr wchar_t kAppName[] = L"Lumen";
constexpr wchar_t kStartUrl[] = L"https://www.wikipedia.org";

struct App {
    lumen::WebKitHost host;
    std::unique_ptr<lumen::WebView> view;
    HWND window { nullptr };
};

// WebKit starts helper processes from the same folder as the executable.
// Check up front so a missing file produces a readable message instead of a
// blank window.
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

void layoutViews(App& app)
{
    if (!app.view)
        return;

    RECT client { };
    GetClientRect(app.window, &client);
    MoveWindow(app.view->hwnd(), 0, 0, client.right, client.bottom, TRUE);
}

void updateTitle(App& app)
{
    if (!app.view)
        return;

    std::wstring title = app.view->title();
    title = title.empty() ? std::wstring(kAppName) : title + L" \u2014 " + kAppName;
    SetWindowTextW(app.window, title.c_str());
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* app = static_cast<App*>(create->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        return TRUE;
    }

    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        std::wstring error;
        if (!app->host.initialize(error)) {
            lumen::log(L"Initialization failed: " + error);
            MessageBoxW(window, error.c_str(), kAppName, MB_OK | MB_ICONERROR);
            return -1;
        }

        app->view = app->host.createView(window);
        if (!app->view) {
            MessageBoxW(window, L"Could not create a web view. See lumen.log.", kAppName, MB_OK | MB_ICONERROR);
            return -1;
        }

        app->view->onChanged = [app] { updateTitle(*app); };
        layoutViews(*app);
        app->view->load(kStartUrl);
        return 0;
    }

    case WM_SIZE:
        if (app)
            layoutViews(*app);
        return 0;

    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        if (app && app->view) {
            const UINT dpi = HIWORD(wParam);
            WKPageSetCustomBackingScaleFactor(app->view->page(), static_cast<double>(dpi) / 96.0);
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 640;
        info->ptMinTrackSize.y = 420;
        return 0;
    }

    case WM_DESTROY:
        if (app)
            app->view.reset();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    lumen::log(L"Starting Lumen.");

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
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        MessageBoxW(nullptr, L"Could not register the window class.", kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }

    App app;
    HWND window = CreateWindowExW(
        0,
        kWindowClass,
        kAppName,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 840,
        nullptr, nullptr, instance, &app);
    if (!window)
        return 1;

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message { };
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
