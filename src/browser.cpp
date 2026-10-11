#include "browser.h"

#include "start_page.h"
#include "util.h"

#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <cwchar>

// Defined in <commctrl.h>, which is not included with WIN32_LEAN_AND_MEAN.
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

namespace lumen {

using namespace gfx;

namespace {

// Metrics in logical (96 dpi) pixels. The layout follows Safari 26's
// "separate tab bar" arrangement: a toolbar row, then a tab bar row that is
// only shown when more than one tab is open.
constexpr int kToolbarHeight = 52;
constexpr int kTabBarHeight = 36;
constexpr int kControlHeight = 34;
constexpr int kToolbarSidePadding = 20;
constexpr int kIconButtonWidth = 38;
constexpr int kGroupGap = 10;
constexpr int kAddressMinWidth = 240;
constexpr int kAddressMaxWidth = 680;
constexpr int kTabBarPadding = 8;
constexpr int kTabGap = 4;
constexpr int kTabMinWidth = 96;
constexpr int kTrafficLightSize = 12;
constexpr int kTrafficLightGap = 8;

bool isStartPage(const std::wstring& url)
{
    return url.empty() || url == L"about:blank";
}

// "https://www.example.com/path?x" becomes "example.com".
std::wstring hostOf(const std::wstring& url)
{
    std::wstring result = url;
    const bool web = result.rfind(L"http://", 0) == 0 || result.rfind(L"https://", 0) == 0;
    if (web) {
        result.erase(0, result.find(L"://") + 3);
        const size_t end = result.find_first_of(L"/?#");
        if (end != std::wstring::npos)
            result.erase(end);
        if (result.rfind(L"www.", 0) == 0)
            result.erase(0, 4);
    }
    return result;
}

wchar_t badgeLetter(const std::wstring& host)
{
    for (wchar_t ch : host) {
        if (ch >= L'a' && ch <= L'z')
            return static_cast<wchar_t>(ch - L'a' + L'A');
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9'))
            return ch;
    }
    return L'?';
}

int hueOf(const std::wstring& host)
{
    unsigned int sum = 0;
    for (wchar_t ch : host)
        sum = sum * 31 + static_cast<unsigned int>(ch);
    return static_cast<int>(sum % 360);
}

std::wstring tabTitle(const WebView& view)
{
    if (!view.title().empty())
        return view.title();
    if (isStartPage(view.url()))
        return L"Start Page";
    const std::wstring host = hostOf(view.url());
    return host.empty() ? std::wstring(L"Start Page") : host;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

Browser::~Browser()
{
    destroy();
    if (m_font)
        DeleteObject(m_font);
    if (m_addressBrush)
        DeleteObject(m_addressBrush);
}

bool Browser::create(HWND window, std::wstring& error)
{
    m_window = window;
    updateFont();
    applyTheme(systemUsesDarkMode());

    if (!m_host.initialize(error))
        return false;

    createAddressControl();

    if (!newTab(L"")) {
        error = L"Could not create the first web view. See lumen.log.";
        return false;
    }
    return true;
}

void Browser::destroy()
{
    m_tabs.clear();
}

// ---------------------------------------------------------------------------
// Metrics, theme, font
// ---------------------------------------------------------------------------

UINT Browser::dpi() const
{
    const UINT value = m_window ? GetDpiForWindow(m_window) : 0;
    return value ? value : 96;
}

int Browser::px(int logical) const
{
    return MulDiv(logical, static_cast<int>(dpi()), 96);
}

float Browser::scaleFactor() const
{
    return static_cast<float>(dpi()) / 96.0f;
}

int Browser::chromeHeight() const
{
    return px(kToolbarHeight) + (m_tabs.size() > 1 ? px(kTabBarHeight) : 0);
}

void Browser::invalidateChrome() const
{
    if (!m_window)
        return;

    RECT area { };
    GetClientRect(m_window, &area);
    area.bottom = std::min<LONG>(area.bottom, px(kToolbarHeight) + px(kTabBarHeight));
    InvalidateRect(m_window, &area, FALSE);
}

void Browser::applyTheme(bool dark)
{
    m_theme = makeTheme(dark);

    if (m_addressBrush)
        DeleteObject(m_addressBrush);
    m_addressBrush = CreateSolidBrush(m_theme.pillFocused);

    if (m_address)
        InvalidateRect(m_address, nullptr, TRUE);
}

void Browser::updateFont()
{
    HFONT font = CreateFontW(
        -MulDiv(13, static_cast<int>(dpi()), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    HFONT previous = m_font;
    m_font = font;
    if (m_address)
        SendMessageW(m_address, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    if (previous)
        DeleteObject(previous);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

ChromeLayout Browser::layout() const
{
    ChromeLayout l;
    RECT client { };
    GetClientRect(m_window, &client);
    const int width = client.right;

    l.toolbarHeight = px(kToolbarHeight);
    l.tabBarHeight = m_tabs.size() > 1 ? px(kTabBarHeight) : 0;
    l.totalHeight = l.toolbarHeight + l.tabBarHeight;

    const int pad = px(kToolbarSidePadding);
    const int light = px(kTrafficLightSize);
    const int lightGap = px(kTrafficLightGap);
    const int control = px(kControlHeight);
    const int controlTop = (l.toolbarHeight - control) / 2;
    const int controlBottom = controlTop + control;
    const int lightTop = (l.toolbarHeight - light) / 2;
    const int button = px(kIconButtonWidth);
    const int groupGap = px(kGroupGap);

    // Window controls (the window has no native title bar).
    l.closeLight = { pad, lightTop, pad + light, lightTop + light };
    l.minimizeLight = {
        l.closeLight.right + lightGap, lightTop,
        l.closeLight.right + lightGap + light, lightTop + light
    };
    l.maximizeLight = {
        l.minimizeLight.right + lightGap, lightTop,
        l.minimizeLight.right + lightGap + light, lightTop + light
    };

    // Left glass groups: sidebar toggle, then back/forward.
    int x = l.maximizeLight.right + px(20);
    l.sidebar = { x, controlTop, x + button + px(4), controlBottom };
    x = l.sidebar.right + groupGap;
    l.navGroup = { x, controlTop, x + button * 2, controlBottom };
    l.back = { l.navGroup.left, controlTop, l.navGroup.left + button, controlBottom };
    l.forward = { l.back.right, controlTop, l.navGroup.right, controlBottom };

    // Right glass group: share, new tab, tab overview.
    l.rightGroup = { width - pad - button * 3, controlTop, width - pad, controlBottom };
    l.shareButton = { l.rightGroup.left, controlTop, l.rightGroup.left + button, controlBottom };
    l.newTabButton = { l.shareButton.right, controlTop, l.shareButton.right + button, controlBottom };
    l.overviewButton = { l.newTabButton.right, controlTop, l.rightGroup.right, controlBottom };

    // Address field: centered in the window when there is room.
    const int minLeft = l.navGroup.right + groupGap * 2;
    const int maxRight = l.rightGroup.left - groupGap * 2;
    const int available = std::max(0, maxRight - minLeft);
    int pillWidth = std::clamp(width * 55 / 100, px(kAddressMinWidth), px(kAddressMaxWidth));
    pillWidth = std::min(pillWidth, available);

    const int centered = (width - pillWidth) / 2;
    const int pillLeft = std::clamp(centered, minLeft, std::max(minLeft, maxRight - pillWidth));
    l.pill = { pillLeft, controlTop, pillLeft + pillWidth, controlBottom };
    l.pageMenu = { l.pill.left, l.pill.top, l.pill.left + px(38), l.pill.bottom };
    l.reload = { l.pill.right - px(38), l.pill.top, l.pill.right, l.pill.bottom };

    // Tab bar (only with more than one tab, like Safari).
    if (l.tabBarHeight > 0) {
        l.tabBar = { 0, l.toolbarHeight, width, l.totalHeight };
        const int outer = px(kTabBarPadding);
        const int gap = px(kTabGap);
        const int count = static_cast<int>(m_tabs.size());
        const int tabWidth = std::max(px(kTabMinWidth), (width - outer * 2 - gap * (count - 1)) / count);
        const int top = l.toolbarHeight + px(4);
        const int bottom = l.totalHeight - px(4);
        for (int i = 0; i < count; ++i) {
            const int left = outer + i * (tabWidth + gap);
            l.tabs.push_back(RECT { left, top, left + tabWidth, bottom });
        }
    }

    return l;
}

RECT Browser::tabCloseRect(const RECT& tab) const
{
    const int size = px(20);
    const int pad = px(6);
    const int top = tab.top + ((tab.bottom - tab.top) - size) / 2;
    return { tab.left + pad, top, tab.left + pad + size, top + size };
}

void Browser::placeAddressBar(const ChromeLayout& l) const
{
    if (!m_address)
        return;

    const int inset = px(16);
    const int height = px(20);
    const int top = l.pill.top + ((l.pill.bottom - l.pill.top) - height) / 2;
    const int width = std::max(px(60), static_cast<int>(l.pill.right - l.pill.left) - inset * 2);
    MoveWindow(m_address, l.pill.left + inset, top, width, height, TRUE);
}

void Browser::resize()
{
    if (!m_window)
        return;

    RECT client { };
    if (!GetClientRect(m_window, &client))
        return;

    const ChromeLayout l = layout();
    placeAddressBar(l);

    for (const auto& tab : m_tabs) {
        HWND viewWindow = tab->hwnd();
        if (!viewWindow)
            continue;

        MoveWindow(
            viewWindow,
            0,
            l.totalHeight,
            client.right,
            std::max<int>(0, static_cast<int>(client.bottom - l.totalHeight)),
            TRUE);
    }

    invalidateChrome();
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

WebView* Browser::active() const
{
    return m_active < m_tabs.size() ? m_tabs[m_active].get() : nullptr;
}

bool Browser::newTab(const std::wstring& url)
{
    auto view = m_host.createView(m_window);
    if (!view)
        return false;

    WebView* raw = view.get();
    raw->onChanged = [this, raw] { onViewChanged(raw); };
    m_tabs.push_back(std::move(view));

    activateTab(m_tabs.size() - 1);

    if (url.empty())
        loadStartPage(*raw);
    else
        raw->load(url);

    return true;
}

void Browser::closeTab(size_t index)
{
    if (index >= m_tabs.size())
        return;

    if (m_tabs.size() == 1) {
        PostMessageW(m_window, WM_CLOSE, 0, 0);
        return;
    }

    m_tabs.erase(m_tabs.begin() + static_cast<std::ptrdiff_t>(index));

    if (m_active >= m_tabs.size())
        m_active = m_tabs.size() - 1;
    else if (m_active > index)
        --m_active;

    activateTab(m_active);
}

void Browser::activateTab(size_t index)
{
    if (index >= m_tabs.size())
        return;

    m_active = index;
    for (size_t i = 0; i < m_tabs.size(); ++i)
        m_tabs[i]->setVisible(i == index);

    resize();
    updateWindowTitle();

    if (!m_editing)
        m_tabs[index]->focus();
    invalidateChrome();
}

void Browser::loadStartPage(WebView& view)
{
    view.loadHtml(startPageHtml(m_theme.dark));
}

void Browser::onViewChanged(WebView* view)
{
    if (view == active())
        updateWindowTitle();
    invalidateChrome();
}

void Browser::updateWindowTitle()
{
    auto* view = active();
    if (!view || !m_window)
        return;

    const std::wstring title = tabTitle(*view) + L" - Lumen";
    SetWindowTextW(m_window, title.c_str());
}

// ---------------------------------------------------------------------------
// Address field
// ---------------------------------------------------------------------------

void Browser::createAddressControl()
{
    m_address = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        0, 0, 10, 10,
        m_window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCmdAddressControl)),
        GetModuleHandleW(nullptr),
        nullptr);
    if (!m_address)
        return;

    SendMessageW(m_address, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    SendMessageW(m_address, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search or enter website name"));

    SetWindowLongPtrW(m_address, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    m_addressOriginal = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(m_address, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Browser::addressProc)));
}

LRESULT CALLBACK Browser::addressProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* browser = reinterpret_cast<Browser*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!browser || !browser->m_addressOriginal)
        return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CHAR:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE)
            return 0;
        break;

    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            browser->commitAddress();
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            browser->cancelAddressEdit();
            return 0;
        }
        break;

    case WM_SETFOCUS: {
        const LRESULT result = CallWindowProcW(browser->m_addressOriginal, window, message, wParam, lParam);
        SendMessageW(window, EM_SETSEL, 0, -1);
        return result;
    }

    case WM_KILLFOCUS: {
        const LRESULT result = CallWindowProcW(browser->m_addressOriginal, window, message, wParam, lParam);
        browser->endAddressEdit();
        return result;
    }

    default:
        break;
    }

    return CallWindowProcW(browser->m_addressOriginal, window, message, wParam, lParam);
}

void Browser::beginAddressEdit()
{
    if (!m_address)
        return;

    if (!m_editing) {
        m_editing = true;

        std::wstring url;
        if (auto* view = active(); view && !isStartPage(view->url()))
            url = view->url();
        SetWindowTextW(m_address, url.c_str());

        placeAddressBar(layout());
        ShowWindow(m_address, SW_SHOW);
    }

    SetFocus(m_address);
    invalidateChrome();
}

void Browser::endAddressEdit()
{
    if (!m_editing)
        return;

    m_editing = false;
    ShowWindow(m_address, SW_HIDE);
    invalidateChrome();
}

void Browser::commitAddress()
{
    const int length = GetWindowTextLengthW(m_address);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(m_address, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));

    navigate(text);

    if (auto* view = active())
        view->focus();
    endAddressEdit();
}

void Browser::cancelAddressEdit()
{
    if (auto* view = active())
        view->focus();
    endAddressEdit();
}

std::wstring Browser::normalizeInput(const std::wstring& input)
{
    std::wstring cleaned = input;
    while (!cleaned.empty() && (cleaned.front() == L' ' || cleaned.front() == L'\t'))
        cleaned.erase(cleaned.begin());
    while (!cleaned.empty() && (cleaned.back() == L' ' || cleaned.back() == L'\t'))
        cleaned.pop_back();

    if (cleaned.empty())
        return { };

    const bool hasScheme = cleaned.find(L"://") != std::wstring::npos
        || cleaned.rfind(L"about:", 0) == 0
        || cleaned.rfind(L"file:", 0) == 0;
    const bool localHost = cleaned.rfind(L"localhost", 0) == 0 || cleaned.rfind(L"127.0.0.1", 0) == 0;
    const bool bareHost = !hasScheme
        && cleaned.find_first_of(L" \t") == std::wstring::npos
        && (cleaned.rfind(L"www.", 0) == 0 || cleaned.find(L'.') != std::wstring::npos);

    if (hasScheme)
        return cleaned;
    if (localHost)
        return L"http://" + cleaned;
    if (bareHost)
        return L"https://" + cleaned;
    return L"https://www.google.com/search?q=" + percentEncode(cleaned);
}

void Browser::navigate(const std::wstring& input)
{
    const std::wstring url = normalizeInput(input);
    if (url.empty())
        return;

    if (auto* view = active())
        view->load(url);
}

// The idle (centered) address text: the bare host, like Safari.
std::wstring Browser::addressDisplayText(bool& secure) const
{
    secure = false;

    auto* view = active();
    if (!view || isStartPage(view->url()))
        return L"";

    secure = view->url().rfind(L"https://", 0) == 0;
    return hostOf(view->url());
}

// ---------------------------------------------------------------------------
// Popups and commands
// ---------------------------------------------------------------------------

void Browser::showPageMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    AppendMenuW(menu, MF_STRING, kCmdNewTab, L"New Tab\tCtrl+T");
    AppendMenuW(menu, MF_STRING, kCmdCloseTab, L"Close Tab\tCtrl+W");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdReload, L"Reload Page\tCtrl+R");
    AppendMenuW(menu, MF_STRING, kCmdStartPage, L"Show Start Page");
    AppendMenuW(menu, MF_STRING, kCmdCopyLink, L"Copy Link");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdZoomIn, L"Zoom In\tCtrl++");
    AppendMenuW(menu, MF_STRING, kCmdZoomOut, L"Zoom Out\tCtrl+-");
    AppendMenuW(menu, MF_STRING, kCmdZoomReset, L"Actual Size\tCtrl+0");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdCloseWindow, L"Quit Lumen");

    const ChromeLayout l = layout();
    POINT point { l.pageMenu.left, l.pill.bottom + px(4) };
    ClientToScreen(m_window, &point);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, point.x, point.y, 0, m_window, nullptr);
    DestroyMenu(menu);
}

void Browser::showTabOverview(const ChromeLayout& l)
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    for (size_t i = 0; i < m_tabs.size(); ++i) {
        std::wstring label = tabTitle(*m_tabs[i]);
        if (label.size() > 48) {
            label.resize(45);
            label += L"...";
        }

        std::wstring escaped;
        for (wchar_t ch : label) {
            escaped += ch;
            if (ch == L'&')
                escaped += L'&';
        }

        UINT flags = MF_STRING;
        if (i == m_active)
            flags |= MF_CHECKED;
        AppendMenuW(menu, flags, static_cast<UINT_PTR>(kCmdTabBase + i), escaped.c_str());
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdNewTab, L"New Tab");

    POINT point { l.overviewButton.right, l.rightGroup.bottom + px(4) };
    ClientToScreen(m_window, &point);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_TOPALIGN, point.x, point.y, 0, m_window, nullptr);
    DestroyMenu(menu);
}

void Browser::copyLink()
{
    auto* view = active();
    if (!view || isStartPage(view->url()))
        return;

    const std::wstring& text = view->url();
    if (!OpenClipboard(m_window))
        return;

    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* data = GlobalLock(memory);
        if (data) {
            std::memcpy(data, text.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
        } else
            GlobalFree(memory);
    }
    CloseClipboard();
}

void Browser::zoomBy(double factor)
{
    if (auto* view = active())
        view->setZoom(view->zoom() * factor);
}

void Browser::onCommand(UINT command)
{
    if (command >= kCmdTabBase && command < kCmdTabBase + m_tabs.size()) {
        activateTab(command - kCmdTabBase);
        return;
    }

    auto* view = active();

    switch (command) {
    case kCmdNewTab:
        newTab(L"");
        break;
    case kCmdCloseTab:
        closeTab(m_active);
        break;
    case kCmdFocusAddress:
        beginAddressEdit();
        break;
    case kCmdReload:
        if (view)
            view->reloadOrStop();
        break;
    case kCmdBack:
        if (view)
            view->goBack();
        break;
    case kCmdForward:
        if (view)
            view->goForward();
        break;
    case kCmdZoomIn:
        zoomBy(1.1);
        break;
    case kCmdZoomOut:
        zoomBy(1.0 / 1.1);
        break;
    case kCmdZoomReset:
        if (view)
            view->setZoom(1.0);
        break;
    case kCmdNextTab:
        if (!m_tabs.empty())
            activateTab((m_active + 1) % m_tabs.size());
        break;
    case kCmdPrevTab:
        if (!m_tabs.empty())
            activateTab((m_active + m_tabs.size() - 1) % m_tabs.size());
        break;
    case kCmdStartPage:
        if (view)
            loadStartPage(*view);
        break;
    case kCmdCopyLink:
        copyLink();
        break;
    case kCmdSidebar:
        MessageBoxW(m_window, L"The sidebar (bookmarks, reading list, history) is planned for a later milestone.", L"Lumen", MB_OK | MB_ICONINFORMATION);
        break;
    case kCmdMinimize:
        ShowWindow(m_window, SW_MINIMIZE);
        break;
    case kCmdMaximize:
        ShowWindow(m_window, IsZoomed(m_window) ? SW_RESTORE : SW_MAXIMIZE);
        break;
    case kCmdCloseWindow:
        PostMessageW(m_window, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Hit testing and input
// ---------------------------------------------------------------------------

HitResult Browser::hitTest(POINT point, const ChromeLayout& l) const
{
    if (point.y < 0 || point.y >= l.totalHeight)
        return { };

    auto contains = [&](const RECT& rect) { return PtInRect(&rect, point) != FALSE; };

    const int slop = px(3);
    RECT lights[3] = { l.closeLight, l.minimizeLight, l.maximizeLight };
    for (auto& rect : lights)
        InflateRect(&rect, slop, slop);

    if (contains(lights[0])) return { Target::CloseWindow, 0 };
    if (contains(lights[1])) return { Target::MinimizeWindow, 0 };
    if (contains(lights[2])) return { Target::MaximizeWindow, 0 };

    if (contains(l.sidebar)) return { Target::Sidebar, 0 };
    if (contains(l.back)) return { Target::Back, 0 };
    if (contains(l.forward)) return { Target::Forward, 0 };

    if (!m_editing) {
        if (contains(l.pageMenu)) return { Target::PageMenu, 0 };
        if (contains(l.reload)) return { Target::Reload, 0 };
    }
    if (contains(l.pill)) return { Target::Address, 0 };

    if (contains(l.shareButton)) return { Target::Share, 0 };
    if (contains(l.newTabButton)) return { Target::NewTab, 0 };
    if (contains(l.overviewButton)) return { Target::TabOverview, 0 };

    for (size_t i = 0; i < l.tabs.size(); ++i) {
        if (!contains(l.tabs[i]))
            continue;

        const RECT close = tabCloseRect(l.tabs[i]);
        if (contains(close))
            return { Target::TabClose, i };
        return { Target::Tab, i };
    }

    return { };
}

bool Browser::hitIsInteractive(POINT point) const
{
    return hitTest(point, layout()).target != Target::None;
}

void Browser::updateHover(POINT point)
{
    const HitResult next = hitTest(point, layout());
    if (next == m_hover)
        return;

    m_hover = next;
    invalidateChrome();
}

void Browser::clearHover()
{
    if (m_hover.target == Target::None)
        return;

    m_hover = HitResult { };
    invalidateChrome();
}

void Browser::onMouseMove(POINT point) { updateHover(point); }
void Browser::onMouseLeave() { clearHover(); }
void Browser::onNcMouseMove() { clearHover(); }

bool Browser::onLeftButtonDown(POINT point)
{
    const ChromeLayout l = layout();
    const HitResult hit = hitTest(point, l);

    if (m_editing && hit.target != Target::Address) {
        SetFocus(m_window);
        endAddressEdit();
    }

    switch (hit.target) {
    case Target::None:
        return false;
    case Target::CloseWindow:
        onCommand(kCmdCloseWindow);
        return true;
    case Target::MinimizeWindow:
        onCommand(kCmdMinimize);
        return true;
    case Target::MaximizeWindow:
        onCommand(kCmdMaximize);
        return true;
    case Target::Sidebar:
        onCommand(kCmdSidebar);
        return true;
    case Target::Back:
        onCommand(kCmdBack);
        return true;
    case Target::Forward:
        onCommand(kCmdForward);
        return true;
    case Target::PageMenu:
        showPageMenu();
        return true;
    case Target::Reload:
        onCommand(kCmdReload);
        return true;
    case Target::Address:
        beginAddressEdit();
        return true;
    case Target::Share:
        onCommand(kCmdCopyLink);
        return true;
    case Target::NewTab:
        onCommand(kCmdNewTab);
        return true;
    case Target::TabOverview:
        showTabOverview(l);
        return true;
    case Target::Tab:
        activateTab(hit.tab);
        return true;
    case Target::TabClose:
        closeTab(hit.tab);
        return true;
    }

    return false;
}

LRESULT Browser::onNcHitTest(LRESULT defaultHit, POINT screenPoint)
{
    // Borders handled by the default procedure (left, right, bottom, corners).
    if (defaultHit != HTCLIENT)
        return defaultHit;

    POINT point = screenPoint;
    ScreenToClient(m_window, &point);

    RECT client { };
    GetClientRect(m_window, &client);

    // The top edge is inside our client area, so provide its resize border.
    if (!IsZoomed(m_window)) {
        const int border = px(6);
        if (point.y >= 0 && point.y < border) {
            if (point.x < border * 2)
                return HTTOPLEFT;
            if (point.x >= client.right - border * 2)
                return HTTOPRIGHT;
            return HTTOP;
        }
    }

    // Unused toolbar and tab bar space drags the window.
    if (point.y >= 0 && point.y < chromeHeight())
        return hitIsInteractive(point) ? HTCLIENT : HTCAPTION;

    return HTCLIENT;
}

void Browser::onSize()
{
    resize();
}

void Browser::onDpiChanged()
{
    for (const auto& tab : m_tabs)
        tab->setBackingScale(static_cast<double>(dpi()) / 96.0);

    updateFont();
    resize();
}

void Browser::onSettingChange(LPARAM lParam)
{
    // Follow the system light/dark preference live, like Safari.
    if (lParam && std::wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
        applyTheme(systemUsesDarkMode());
        invalidateChrome();
    }
}

void Browser::onSetFocus()
{
    if (m_editing)
        return;

    if (auto* view = active())
        view->focus();
}

bool Browser::onMouseWheel(WPARAM wParam, LPARAM lParam)
{
    // If the main window receives the wheel (focus was not on the page),
    // hand it to the page. The guard stops a forwarding loop, since an
    // unhandled wheel message is passed back up to this window.
    if (m_forwardingWheel)
        return false;

    auto* view = active();
    HWND target = view ? view->hwnd() : nullptr;
    if (!target)
        return false;

    m_forwardingWheel = true;
    SendMessageW(target, WM_MOUSEWHEEL, wParam, lParam);
    m_forwardingWheel = false;
    return true;
}

LRESULT Browser::onCtlColorEdit(HDC dc, HWND control)
{
    if (control != m_address)
        return 0;

    SetTextColor(dc, m_theme.text);
    SetBkColor(dc, m_theme.pillFocused);
    return reinterpret_cast<LRESULT>(m_addressBrush);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void Browser::onPaint(HDC target)
{
    RECT client { };
    GetClientRect(m_window, &client);

    const ChromeLayout l = layout();

    // Page area behind the web views.
    const RECT below { 0, l.totalHeight, client.right, client.bottom };
    fillSolid(target, below, m_theme.page);

    // The chrome is drawn into an off-screen bitmap to avoid flicker.
    const int width = client.right;
    const int height = l.totalHeight;
    if (width <= 0 || height <= 0)
        return;

    HDC memory = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ previous = SelectObject(memory, bitmap);

    paintChrome(memory, l);
    BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);

    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
}

void Browser::paintChrome(HDC dc, const ChromeLayout& l)
{
    RECT client { };
    GetClientRect(m_window, &client);
    const int width = client.right;
    const float k = scaleFactor();

    fillSolid(dc, RECT { 0, 0, width, l.toolbarHeight }, m_theme.toolbar);
    if (l.tabBarHeight > 0)
        fillSolid(dc, l.tabBar, m_theme.tabBar);
    fillSolid(dc, RECT { 0, l.totalHeight - 1, width, l.totalHeight }, m_theme.separator);

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    WebView* current = active();
    const bool canGoBack = current && current->canGoBack();
    const bool canGoForward = current && current->canGoForward();
    const double progress = current ? current->progress() : 1.0;
    const bool loading = current && progress > 0.0 && progress < 1.0;

    auto drawText = [&](const std::wstring& text, RECT rect, COLORREF color, UINT flags) {
        g.Flush(Gdiplus::FlushIntentionSync);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        HFONT oldFont = static_cast<HFONT>(SelectObject(dc, m_font));
        DrawTextW(dc, text.c_str(), -1, &rect, flags);
        SelectObject(dc, oldFont);
    };
    auto measureText = [&](const std::wstring& text) {
        g.Flush(Gdiplus::FlushIntentionSync);
        RECT rect { 0, 0, 0, 0 };
        HFONT oldFont = static_cast<HFONT>(SelectObject(dc, m_font));
        DrawTextW(dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        return static_cast<int>(rect.right - rect.left);
    };

    // --- window controls ---
    const bool lightsHot = m_hover.target == Target::CloseWindow
        || m_hover.target == Target::MinimizeWindow
        || m_hover.target == Target::MaximizeWindow;
    drawTrafficLight(g, l.closeLight, RGB(255, 95, 87));
    drawTrafficLight(g, l.minimizeLight, RGB(254, 188, 46));
    drawTrafficLight(g, l.maximizeLight, RGB(40, 200, 64));
    if (lightsHot) {
        drawTrafficLightGlyph(g, l.closeLight, TrafficLight::Close, k);
        drawTrafficLightGlyph(g, l.minimizeLight, TrafficLight::Minimize, k);
        drawTrafficLightGlyph(g, l.maximizeLight, TrafficLight::Maximize, k);
    }

    // --- glass capsules ---
    auto drawGlass = [&](const RECT& rect) {
        const int radius = (rect.bottom - rect.top) / 2;
        fillRoundedRect(g, rect, radius, m_theme.glass);
        strokeRoundedRect(g, rect, radius, m_theme.glassBorder, std::max(1.0f, k));
    };
    auto drawHot = [&](const RECT& rect) {
        RECT inset = rect;
        InflateRect(&inset, -px(2), -px(2));
        fillRoundedRect(g, inset, (inset.bottom - inset.top) / 2, m_theme.glassHover);
    };

    drawGlass(l.sidebar);
    if (m_hover.target == Target::Sidebar)
        drawHot(l.sidebar);
    drawSidebarIcon(g, l.sidebar, m_theme.text, k);

    drawGlass(l.navGroup);
    if (m_hover.target == Target::Back && canGoBack)
        drawHot(l.back);
    if (m_hover.target == Target::Forward && canGoForward)
        drawHot(l.forward);
    drawChevronIcon(g, l.back, false, canGoBack ? m_theme.text : m_theme.textDisabled, k);
    drawChevronIcon(g, l.forward, true, canGoForward ? m_theme.text : m_theme.textDisabled, k);

    drawGlass(l.rightGroup);
    if (m_hover.target == Target::Share)
        drawHot(l.shareButton);
    if (m_hover.target == Target::NewTab)
        drawHot(l.newTabButton);
    if (m_hover.target == Target::TabOverview)
        drawHot(l.overviewButton);
    drawShareIcon(g, l.shareButton, m_theme.text, k);
    drawPlusIcon(g, l.newTabButton, m_theme.text, k);
    drawTabOverviewIcon(g, l.overviewButton, m_theme.text, k);

    // --- address field ---
    const int pillRadius = (l.pill.bottom - l.pill.top) / 2;
    const bool pillHot = m_hover.target == Target::Address
        || m_hover.target == Target::PageMenu
        || m_hover.target == Target::Reload;
    const COLORREF pillColor = m_editing ? m_theme.pillFocused : (pillHot ? m_theme.pillHover : m_theme.pill);
    fillRoundedRect(g, l.pill, pillRadius, pillColor);

    if (!m_editing && loading) {
        Gdiplus::GraphicsPath clipPath;
        addRoundedRect(clipPath, toRectF(l.pill), static_cast<Gdiplus::REAL>(pillRadius));
        g.SetClip(&clipPath);

        RECT bar = l.pill;
        const double shown = std::clamp(progress, 0.06, 1.0);
        bar.right = bar.left + static_cast<LONG>((l.pill.right - l.pill.left) * shown);
        Gdiplus::SolidBrush progressBrush(toGdiColor(m_theme.accent, m_theme.dark ? 90 : 60));
        g.FillRectangle(&progressBrush, toRectF(bar));
        g.ResetClip();
    }

    if (m_editing) {
        strokeRoundedRect(g, l.pill, pillRadius, m_theme.accent, 2.0f * k, 210);
    } else {
        drawPageMenuIcon(g, l.pageMenu, m_theme.textSecondary, k);
        if (loading)
            drawStopIcon(g, l.reload, m_theme.textSecondary, k);
        else
            drawReloadIcon(g, l.reload, m_theme.textSecondary, k);

        bool secure = false;
        const std::wstring host = addressDisplayText(secure);
        const bool hasPage = !host.empty();
        const std::wstring label = hasPage ? host : std::wstring(L"Search or enter website name");

        RECT area { l.pageMenu.right, l.pill.top, l.reload.left, l.pill.bottom };
        const int areaWidth = area.right - area.left;
        const int lockWidth = secure ? px(14) : 0;
        const int textWidth = std::min(measureText(label), std::max(0, areaWidth - lockWidth));
        const int groupWidth = lockWidth + textWidth;
        const int groupLeft = area.left + (areaWidth - groupWidth) / 2;

        if (secure) {
            drawLockIcon(
                g,
                static_cast<float>(groupLeft) + 5.0f * k,
                static_cast<float>((l.pill.top + l.pill.bottom) / 2),
                m_theme.textSecondary,
                k);
        }

        const RECT textRect { groupLeft + lockWidth, l.pill.top, groupLeft + groupWidth, l.pill.bottom };
        drawText(
            label,
            textRect,
            hasPage ? m_theme.text : m_theme.textSecondary,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // --- tab bar ---
    for (size_t i = 0; i < l.tabs.size(); ++i) {
        const RECT& tabRect = l.tabs[i];
        const bool isActive = i == m_active;
        const bool hot = (m_hover.target == Target::Tab || m_hover.target == Target::TabClose) && m_hover.tab == i;
        const int tabRadius = px(8);

        if (isActive) {
            fillRoundedRect(g, tabRect, tabRadius, m_theme.tabActive);
            strokeRoundedRect(g, tabRect, tabRadius, m_theme.tabBorder, std::max(1.0f, k), 150);
        } else if (hot) {
            fillRoundedRect(g, tabRect, tabRadius, m_theme.tabHover);
        }

        if (isActive || hot) {
            const RECT closeRect = tabCloseRect(tabRect);
            if (m_hover.target == Target::TabClose && m_hover.tab == i)
                fillRoundedRect(g, closeRect, (closeRect.bottom - closeRect.top) / 2, m_theme.glassHover);
            drawCloseIcon(g, closeRect, m_theme.textSecondary, k);
        }

        const WebView& view = *m_tabs[i];
        const int sideInset = px(30);
        const int iconSize = px(14);
        const int iconGap = px(6);
        const RECT area { tabRect.left + sideInset, tabRect.top, tabRect.right - sideInset, tabRect.bottom };
        const int areaWidth = std::max(0, static_cast<int>(area.right - area.left));

        const std::wstring title = tabTitle(view);
        const int availableText = std::max(0, areaWidth - iconSize - iconGap);
        const int textWidth = std::min(measureText(title), availableText);
        const int groupWidth = iconSize + iconGap + textWidth;
        const int groupLeft = area.left + (areaWidth - groupWidth) / 2;
        const int centerY = (tabRect.top + tabRect.bottom) / 2;

        const RECT iconRect { groupLeft, centerY - iconSize / 2, groupLeft + iconSize, centerY - iconSize / 2 + iconSize };
        if (isStartPage(view.url())) {
            drawStartPageIcon(g, iconRect, m_theme.textSecondary, k);
        } else {
            const std::wstring host = hostOf(view.url());
            drawSiteBadge(g, iconRect, badgeLetter(host), colorFromHue(hueOf(host)), k);
        }

        const RECT textRect { iconRect.right + iconGap, tabRect.top, iconRect.right + iconGap + textWidth, tabRect.bottom };
        drawText(
            title,
            textRect,
            isActive ? m_theme.text : m_theme.textSecondary,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    g.Flush(Gdiplus::FlushIntentionSync);
}

} // namespace lumen
