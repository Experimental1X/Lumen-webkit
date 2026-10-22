#include "dialogs.h"

#include <commdlg.h>

namespace lumen {

namespace {

constexpr wchar_t kPromptClass[] = L"LumenPromptWindow";
constexpr int kPromptEdit = 201;

struct PromptState {
    std::wstring message;
    std::wstring value;
    bool accepted { false };
    bool done { false };
};

LRESULT CALLBACK promptProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wParam, lParam);
    }

    auto* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
    case WM_COMMAND:
        if (state && LOWORD(wParam) == IDOK) {
            HWND edit = GetDlgItem(window, kPromptEdit);
            const int length = GetWindowTextLengthW(edit);
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(edit, text.data(), length + 1);
            text.resize(static_cast<size_t>(length));
            state->value = text;
            state->accepted = true;
            state->done = true;
            return 0;
        }
        if (state && LOWORD(wParam) == IDCANCEL) {
            state->done = true;
            return 0;
        }
        break;

    case WM_CLOSE:
        if (state)
            state->done = true;
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

void showAlert(HWND parent, const std::wstring& message)
{
    MessageBoxW(parent, message.c_str(), L"Lumen", MB_OK | MB_ICONINFORMATION);
}

bool showConfirm(HWND parent, const std::wstring& message)
{
    return MessageBoxW(parent, message.c_str(), L"Lumen", MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
}

bool showPrompt(HWND parent, const std::wstring& message, const std::wstring& defaultValue, std::wstring& result)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass { };
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = promptProcedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        windowClass.lpszClassName = kPromptClass;
        RegisterClassExW(&windowClass);
        registered = true;
    }

    const UINT dpi = parent ? GetDpiForWindow(parent) : 96;
    auto scale = [&](int value) { return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96); };

    PromptState state;
    state.message = message;

    const int width = scale(440);
    const int height = scale(210);

    RECT parentRect { };
    if (!parent || !GetWindowRect(parent, &parentRect))
        parentRect = RECT { 100, 100, 100 + width, 100 + height };
    const int x = parentRect.left + ((parentRect.right - parentRect.left) - width) / 2;
    const int y = parentRect.top + ((parentRect.bottom - parentRect.top) - height) / 3;

    HWND window = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kPromptClass, L"Lumen",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, width, height,
        parent, nullptr, GetModuleHandleW(nullptr), &state);
    if (!window)
        return false;

    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const int margin = scale(16);

    RECT client { };
    GetClientRect(window, &client);
    const int clientWidth = static_cast<int>(client.right);
    const int buttonWidth = scale(80);

    HWND label = CreateWindowExW(0, L"STATIC", message.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
        margin, margin, clientWidth - margin * 2, scale(44), window, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", defaultValue.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        margin, scale(64), clientWidth - margin * 2, scale(24), window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPromptEdit)), GetModuleHandleW(nullptr), nullptr);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        clientWidth - margin - buttonWidth * 2 - scale(8), scale(106), buttonWidth, scale(28), window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), GetModuleHandleW(nullptr), nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        clientWidth - margin - buttonWidth, scale(106), buttonWidth, scale(28), window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), GetModuleHandleW(nullptr), nullptr);

    for (HWND control : { label, edit, ok, cancel })
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    if (parent)
        EnableWindow(parent, FALSE);
    ShowWindow(window, SW_SHOW);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);

    MSG message_ { };
    while (!state.done) {
        const BOOL status = GetMessageW(&message_, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0)
                PostQuitMessage(static_cast<int>(message_.wParam));
            break;
        }
        if (!IsDialogMessageW(window, &message_)) {
            TranslateMessage(&message_);
            DispatchMessageW(&message_);
        }
    }

    if (parent)
        EnableWindow(parent, TRUE);
    DestroyWindow(window);
    if (parent)
        SetForegroundWindow(parent);

    if (state.accepted)
        result = state.value;
    return state.accepted;
}

std::vector<std::wstring> chooseFiles(HWND parent, bool allowMultiple)
{
    std::vector<wchar_t> buffer(65536, L'\0');

    OPENFILENAMEW dialog { };
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = parent;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (allowMultiple ? OFN_ALLOWMULTISELECT : 0);

    std::vector<std::wstring> files;
    if (!GetOpenFileNameW(&dialog))
        return files;

    // With several files the buffer holds "folder\0name1\0name2\0\0"; with
    // one file it holds just the full path.
    const std::wstring first(buffer.data());
    const wchar_t* next = buffer.data() + first.size() + 1;
    if (*next == L'\0') {
        files.push_back(first);
        return files;
    }

    while (*next != L'\0') {
        const std::wstring name(next);
        files.push_back(first + L"\\" + name);
        next += name.size() + 1;
    }
    return files;
}

} // namespace lumen
