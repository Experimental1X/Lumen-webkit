#include "browser.h"

#include "dialogs.h"
#include "downloads.h"
#include "downloads_page.h"
#include "settings.h"
#include "settings_page.h"
#include "start_page.h"
#include "util.h"

#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
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
constexpr int kSidebarWidth = 280;
constexpr int kFindBarHeight = 46;

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
    releaseBackBuffer();
    releaseOverviewSurface();
    if (m_font)
        DeleteObject(m_font);
    if (m_addressBrush)
        DeleteObject(m_addressBrush);
    if (m_findBrush)
        DeleteObject(m_findBrush);
}

bool Browser::create(HWND window, std::wstring& error)
{
    m_window = window;
    updateFont();
    applyTheme(systemUsesDarkMode());
    m_library.load();

    if (!m_host.initialize(error))
        return false;

    createAddressControl();
    createFindControl();
    createDownloadsPanel();

    // Nothing is mid-transition at startup: every spring starts parked at the
    // value its target already has, so the first paint is not a fade-in.
    m_sidebarAnim.snap(0.0);
    m_panelAnim.snap(0.0);
    m_overviewAnim.snap(0.0);
    m_findAnim.snap(0.0);
    m_addressAnim.snap(0.0);
    m_pressAnim.snap(0.0);
    Downloads::instance().setOwnerWindow(m_window);
    // Settings persist across launches (settings.tsv under %APPDATA%\Lumen).
    Settings::instance().load();
    // The download callbacks can fire on WebKit's IPC threads; funnel them
    // through WM_LUMEN_DOWNLOAD_CHANGED so onDownloadsChanged always runs on
    // the UI thread (its code touches GDI).
    Downloads::instance().onChanged = [this](bool started) {
        PostMessageW(m_window, WM_LUMEN_DOWNLOAD_CHANGED, started ? 1 : 0, 0);
    };

    if (!newTab(L"")) {
        error = L"Could not create the first web view. See lumen.log.";
        return false;
    }
    return true;
}

void Browser::destroy()
{
    Downloads::instance().onChanged = nullptr;
    if (m_window)
        KillTimer(m_window, kReapTimer);
    m_tabs.clear();
    m_closedTabs.clear();
}

void Browser::onTimer(UINT_PTR id)
{
    if (id == kReapTimer)
        reapTabs();
    else if (id == kProgressTimer)
        Downloads::instance().refreshFromDisk();
    else if (id == kAnimTimer)
        stepAnimations();
}

// ---------------------------------------------------------------------------
// Motion
// ---------------------------------------------------------------------------

bool Browser::Spring::step(double dt, double response, double damping)
{
    if (settled())
        return false;

    // Moving the page view is the expensive part of an animation tick - every
    // width change makes WebKit re-lay-out and re-raster the whole viewport -
    // so consecutive ticks can easily land 150-250 ms apart. Handing that whole
    // gap to a single Euler step would fling the spring off screen, but
    // clamping it to one 30 ms step is worse than unstable: the spring then
    // advances 30 ms of motion per 250 ms of wall clock and a half-second
    // transition stretches out over nine seconds, which is exactly what the
    // screen recording shows.
    //
    // So walk the elapsed time instead of truncating it. Every substep is
    // small enough for the integrator to stay stable, and together they cover
    // the real gap, so the animation finishes in the time the constants ask
    // for no matter how slow the frames are.
    dt = std::min(dt, 0.5);
    if (dt <= 0.0)
        return true;

    // Damped harmonic oscillator: a = -w^2(x - target) - 2*zeta*w*v
    const double omega = 2.0 * 3.14159265358979323846 / response;
    const double stiffness = omega * omega;
    const double drag = 2.0 * damping * omega;

    constexpr double kSubstep = 1.0 / 120.0;
    while (dt > 0.0) {
        const double h = std::min(dt, kSubstep);
        dt -= h;

        velocity += (-stiffness * (value - target) - drag * velocity) * h;
        value += velocity * h;

        // Snap the asymptote so the timer can actually stop. Sub-pixel, so
        // nothing visibly jumps when it lands.
        if (std::abs(value - target) < 0.05 && std::abs(velocity) < 0.5) {
            value = target;
            velocity = 0.0;
            return false;
        }
    }
    return true;
}

void Browser::syncAnimations()
{
    m_sidebarAnim.target = sidebarVisibleNow() ? static_cast<double>(px(kSidebarWidth)) : 0.0;
    m_panelAnim.target = m_downloadsVisible ? 1.0 : 0.0;
    m_overviewAnim.target = m_overview ? 1.0 : 0.0;
    m_findAnim.target = m_findVisible ? 1.0 : 0.0;
    m_addressAnim.target = m_editing ? 1.0 : 0.0;
    m_pressAnim.target = m_pressTarget >= 0 ? 1.0 : 0.0;
    ensureAnimTimer();
}

void Browser::ensureAnimTimer()
{
    if (!m_window)
        return;

    const bool busy = !m_sidebarAnim.settled() || !m_panelAnim.settled()
        || !m_overviewAnim.settled() || !m_findAnim.settled()
        || !m_addressAnim.settled() || !m_pressAnim.settled();

    if (busy)
        SetTimer(m_window, kAnimTimer, 16, nullptr);
    else {
        KillTimer(m_window, kAnimTimer);
        m_lastAnimTick = 0;
    }
}

bool Browser::stepAnimations()
{
    const ULONGLONG now = GetTickCount64();
    if (m_lastAnimTick == 0)
        m_lastAnimTick = now;

    const double dt = static_cast<double>(now - m_lastAnimTick) / 1000.0;
    m_lastAnimTick = now;

    bool moving = false;
    moving |= m_sidebarAnim.step(dt, kLayoutResponse, kLayoutDamping);
    moving |= m_panelAnim.step(dt, kLayoutResponse, kLayoutDamping);
    moving |= m_overviewAnim.step(dt, kLayoutResponse, kLayoutDamping);
    moving |= m_findAnim.step(dt, kLayoutResponse, kLayoutDamping);
    moving |= m_addressAnim.step(dt, kControlResponse, kControlDamping);
    moving |= m_pressAnim.step(dt, kControlResponse, kControlDamping);

    // The downloads panel is a separate top-level window, so it cannot simply
    // fade out - it has to shrink to nothing and then be hidden for real.
    const bool panelLanded = m_panelAnim.settled() && m_panelAnim.value <= 0.0;
    if (panelLanded && m_downloadsPanel && !m_downloadsVisible)
        ShowWindow(m_downloadsPanel, SW_HIDE);

    // Same story for the find bar's edit control.
    if (m_findAnim.settled() && m_findAnim.value <= 0.0 && !m_findVisible && m_find)
        ShowWindow(m_find, SW_HIDE);

    // The tab overview hides every tab view while it is up, so the page only
    // comes back once the overlay has finished easing away.
    if (!m_overview && m_overviewAnim.settled() && m_overviewAnim.value <= 0.0 && m_active < m_tabs.size())
        m_tabs[m_active]->setVisible(true);

    if (moving) {
        resize(false);
        if (m_downloadsPanel && (m_downloadsVisible || m_panelAnim.value > 0.0))
            layoutDownloadsPanel();
        invalidateChrome();
    } else
        ensureAnimTimer();

    return moving;
}

int Browser::sidebarWidthNow() const
{
    if (!m_sidebarAnim.settled())
        return std::clamp(static_cast<int>(std::lround(m_sidebarAnim.value)), 0, px(kSidebarWidth));
    return m_sidebarAnim.target > 0.0 ? px(kSidebarWidth) : 0;
}

void Browser::reapTabs()
{
    // Release tabs that have been closed for a while, long after the message
    // that closed them and after WebKit had time to shut their pages down.
    const ULONGLONG now = GetTickCount64();
    for (auto it = m_closedTabs.begin(); it != m_closedTabs.end();) {
        if (now - it->closedAt >= 5000) {
            log(L"Releasing a closed tab.");
            it = m_closedTabs.erase(it);
        } else
            ++it;
    }

    if (m_closedTabs.empty() && m_window)
        KillTimer(m_window, kReapTimer);
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
    // The find bar is the one piece of chrome that changes the page's top edge,
    // so it has to travel: a bar that appears at full height instantly reads as
    // a layout jump rather than a transition.
    const int findHeight = static_cast<int>(std::lround(m_findAnim.value * px(kFindBarHeight)));
    return px(kToolbarHeight) + (m_tabs.size() > 1 ? px(kTabBarHeight) : 0) + findHeight;
}

void Browser::invalidateChrome() const
{
    if (!m_window)
        return;

    RECT area { };
    GetClientRect(m_window, &area);
    area.bottom = std::min<LONG>(area.bottom, px(kToolbarHeight) + px(kTabBarHeight) + px(kFindBarHeight));
    InvalidateRect(m_window, &area, FALSE);
}

void Browser::invalidateSidebarColumn()
{
    if (!m_window)
        return;

    // Everything the sidebar draws itself lives in one column: the full
    // current width, from under the chrome to the bottom of the client area.
    // Invalidating only that column is what keeps a sidebar animation from
    // flashing - a whole-window invalidate also repaints the page area behind
    // the web view, whose background fill gets cleared and refilled every tick.
    const int width = sidebarWidthNow();
    if (width <= 0)
        return;

    RECT client { };
    GetClientRect(m_window, &client);

    RECT column { 0, chromeHeight(), width, std::max<LONG>(0, client.bottom) };
    InvalidateRect(m_window, &column, FALSE);
}

void Browser::applyTheme(bool dark)
{
    m_theme = makeTheme(dark);

    if (m_addressBrush)
        DeleteObject(m_addressBrush);
    m_addressBrush = CreateSolidBrush(m_theme.pillFocused);

    if (m_findBrush)
        DeleteObject(m_findBrush);
    m_findBrush = CreateSolidBrush(m_theme.pill);

    if (m_address)
        InvalidateRect(m_address, nullptr, TRUE);
    if (m_find)
        InvalidateRect(m_find, nullptr, TRUE);
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
    if (m_find)
        SendMessageW(m_find, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
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
    l.findBarHeight = m_findVisible ? px(kFindBarHeight) : 0;
    l.totalHeight = l.toolbarHeight + l.tabBarHeight + l.findBarHeight;

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
    l.hasDownloads = !Downloads::instance().items().empty();
    const int rightButtons = l.hasDownloads ? 4 : 3;
    l.rightGroup = { width - pad - button * rightButtons, controlTop, width - pad, controlBottom };
    int buttonX = l.rightGroup.left;
    if (l.hasDownloads) {
        l.downloadsButton = { buttonX, controlTop, buttonX + button, controlBottom };
        buttonX += button;
    }
    l.shareButton = { buttonX, controlTop, buttonX + button, controlBottom };
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

    // Find bar (a third row, shown with Ctrl+F).
    if (l.findBarHeight > 0) {
        const int top = l.toolbarHeight + l.tabBarHeight;
        l.findBar = { 0, top, width, l.totalHeight };
        const int fieldHeight = px(30);
        const int fieldTop = top + (l.findBarHeight - fieldHeight) / 2;
        const int fieldWidth = std::clamp(width - px(360), px(140), px(320));
        const int step = px(32);
        l.findField = { pad, fieldTop, pad + fieldWidth, fieldTop + fieldHeight };
        l.findPrevious = { l.findField.right + px(8), fieldTop, l.findField.right + px(8) + step, fieldTop + fieldHeight };
        l.findNext = { l.findPrevious.right, fieldTop, l.findPrevious.right + step, fieldTop + fieldHeight };
        l.findStatus = { l.findNext.right + px(12), fieldTop, l.findNext.right + px(12) + px(150), fieldTop + fieldHeight };
        l.findDone = { width - pad - px(64), fieldTop, width - pad, fieldTop + fieldHeight };
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

void Browser::resize(bool wholeClient)
{
    if (!m_window)
        return;

    RECT client { };
    if (!GetClientRect(m_window, &client))
        return;

    const ChromeLayout l = layout();
    placeAddressBar(l);
    placeFindEdit(l);

    const int viewLeft = sidebarWidthNow();
    const int viewWidth = std::max<int>(0, static_cast<int>(client.right) - viewLeft);
    const int viewHeight = std::max<int>(0, static_cast<int>(client.bottom - l.totalHeight));

    for (const auto& tab : m_tabs) {
        HWND viewWindow = tab->hwnd();
        if (!viewWindow)
            continue;

        // Moving the view already repaints it and invalidates the strip it
        // vacated, so only the chrome we own ourselves needs a nudge. Handing
        // MoveWindow a geometry it already has would still cost a full
        // WebKit re-layout, and an animation runs this a dozen times a second.
        RECT current { };
        GetWindowRect(viewWindow, &current);
        MapWindowPoints(HWND_DESKTOP, m_window, reinterpret_cast<POINT*>(&current), 2);
        if (current.left == viewLeft && current.top == l.totalHeight
            && current.right - current.left == viewWidth
            && current.bottom - current.top == viewHeight)
            continue;

        MoveWindow(viewWindow, viewLeft, l.totalHeight, viewWidth, viewHeight, TRUE);
    }

    // The sidebar and the tab overview are drawn by this window and have to
    // follow every size change. Only the page area is somebody else's, and
    // repainting it means clearing the fill behind the web view and watching
    // it flash - so an animation tick leaves it alone and lets MoveWindow's
    // own repaint deal with the strip that opened up.
    if (wholeClient)
        InvalidateRect(m_window, nullptr, FALSE);
    else
        invalidateSidebarColumn();

    if (m_downloadsVisible || m_panelAnim.value > 0.0)
        layoutDownloadsPanel();
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
    // Opening a tab leaves the overview; activateTab shows the right view.
    m_overview = false;

    auto view = m_host.createView(m_window);
    if (!view)
        return false;

    WebView* raw = view.get();
    attachCallbacks(raw);
    m_tabs.push_back(std::move(view));

    activateTab(m_tabs.size() - 1);

    if (url.empty())
        loadStartPage(*raw);
    else
        raw->load(url);

    InvalidateRect(m_window, nullptr, FALSE);
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

    // Take the tab out of the list first, so nothing that runs while it is
    // being hidden can reach it, and destroy it later (see reapTabs). Destroying
    // a web view in the middle of the message that closed it was crashing.
    std::unique_ptr<WebView> closing = std::move(m_tabs[index]);
    m_tabs.erase(m_tabs.begin() + static_cast<std::ptrdiff_t>(index));

    if (m_downloadsPageTab != static_cast<size_t>(-1)) {
        if (m_downloadsPageTab == index)
            m_downloadsPageTab = static_cast<size_t>(-1);
        else if (m_downloadsPageTab > index)
            --m_downloadsPageTab;
    }

    if (m_active >= m_tabs.size())
        m_active = m_tabs.size() - 1;
    else if (m_active > index)
        --m_active;

    m_hover = HitResult { };
    m_overviewHoverCard = -1;
    m_overviewHoverClose = false;

    closing->onChanged = nullptr;
    closing->setVisible(false);
    closing->closePage();
    log(L"Tab closed; it will be released in a few seconds.");
    m_closedTabs.push_back(ClosedTab { std::move(closing), GetTickCount64() });
    SetTimer(m_window, kReapTimer, 2000, nullptr);

    activateTab(m_active);
    InvalidateRect(m_window, nullptr, FALSE);
}

void Browser::activateTab(size_t index)
{
    if (index >= m_tabs.size())
        return;

    // Remember what the tab we are leaving looks like, for the overview.
    if (!m_overview && index != m_active) {
        WebView* leaving = active();
        if (leaving && leaving->hwnd() && IsWindowVisible(leaving->hwnd()) && !IsIconic(m_window))
            captureThumbnail(*leaving);
    }

    m_active = index;
    for (size_t i = 0; i < m_tabs.size(); ++i)
        m_tabs[i]->setVisible(!m_overview && i == index);

    resize();
    updateWindowTitle();

    if (m_overview)
        SetFocus(m_window);
    else if (!m_editing)
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

    if (view && m_library.visit(view->url(), view->title()) && m_sidebarVisible && m_sidebarMode == 2 && !m_overview)
        InvalidateRect(m_window, nullptr, FALSE);

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
        syncAnimations();
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
    syncAnimations();
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
    if (!view)
        return L"";

    // The downloads page is loaded via loadHtml and so its URL reads as
    // "about:blank"; show its title in the address bar instead.
    if (view->title() == L"Downloads")
        return L"Downloads";

    if (isStartPage(view->url()))
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

    auto* view = active();
    const int zoomPercent = view ? static_cast<int>(view->zoom() * 100.0 + 0.5) : 100;

    AppendMenuW(menu, MF_STRING, kCmdNewTab, L"New Tab\tCtrl+T");
    AppendMenuW(menu, MF_STRING, kCmdCloseTab, L"Close Tab\tCtrl+W");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdSidebar, m_sidebarVisible ? L"Hide Sidebar\tCtrl+Shift+L" : L"Show Sidebar\tCtrl+Shift+L");
    AppendMenuW(menu, MF_STRING, kCmdTabOverview, m_overview ? L"Hide Tab Overview\tCtrl+Shift+\\" : L"Show Tab Overview\tCtrl+Shift+\\");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdBookmarkPage, L"Add Bookmark\tCtrl+D");
    AppendMenuW(menu, MF_STRING, kCmdReadingList, L"Add to Reading List\tCtrl+Shift+D");
    AppendMenuW(menu, MF_STRING, kCmdCopyLink, L"Copy Link");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdFind, L"Find on Page...\tCtrl+F");
    AppendMenuW(menu, MF_STRING, kCmdDownloads, L"Show Downloads\tCtrl+J");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdReload, L"Reload Page\tCtrl+R");
    AppendMenuW(menu, MF_STRING, kCmdStartPage, L"Show Start Page");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, (L"Zoom: " + std::to_wstring(zoomPercent) + L"%").c_str());
    AppendMenuW(menu, MF_STRING, kCmdZoomIn, L"Zoom In\tCtrl++");
    AppendMenuW(menu, MF_STRING, kCmdZoomOut, L"Zoom Out\tCtrl+-");
    AppendMenuW(menu, MF_STRING, kCmdZoomReset, L"Actual Size\tCtrl+0");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdClearHistory, L"Clear History");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdCloseWindow, L"Quit Lumen");

    const ChromeLayout l = layout();
    POINT point { l.pageMenu.left, l.pill.bottom + px(4) };
    ClientToScreen(m_window, &point);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, point.x, point.y, 0, m_window, nullptr);
    DestroyMenu(menu);
}

void Browser::showShareMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    AppendMenuW(menu, MF_STRING, kCmdCopyLink, L"Copy Link");
    AppendMenuW(menu, MF_STRING, kCmdBookmarkPage, L"Add Bookmark");
    AppendMenuW(menu, MF_STRING, kCmdReadingList, L"Add to Reading List");

    const ChromeLayout l = layout();
    POINT point { l.shareButton.left, l.rightGroup.bottom + px(4) };
    ClientToScreen(m_window, &point);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, point.x, point.y, 0, m_window, nullptr);
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
        toggleSidebar();
        break;
    case kCmdTabOverview:
        showOverview();
        break;
    case kCmdBookmarkPage:
        bookmarkCurrent();
        break;
    case kCmdReadingList:
        readingListCurrent();
        break;
    case kCmdClearHistory:
        m_library.clearHistory();
        InvalidateRect(m_window, nullptr, FALSE);
        break;
    case kCmdFind:
        showFind();
        break;
    case kCmdFindNext:
        if (m_findVisible)
            runFind(false, false);
        else
            showFind();
        break;
    case kCmdFindPrevious:
        if (m_findVisible)
            runFind(true, false);
        else
            showFind();
        break;
    case kCmdDownloads:
        toggleDownloadsPanel();
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

    if (l.hasDownloads && contains(l.downloadsButton)) return { Target::Downloads, 0 };
    if (contains(l.shareButton)) return { Target::Share, 0 };
    if (contains(l.newTabButton)) return { Target::NewTab, 0 };
    if (contains(l.overviewButton)) return { Target::TabOverview, 0 };

    if (l.findBarHeight > 0) {
        if (contains(l.findPrevious)) return { Target::FindPrevious, 0 };
        if (contains(l.findNext)) return { Target::FindNext, 0 };
        if (contains(l.findDone)) return { Target::FindDone, 0 };
    }

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

void Browser::onMouseMove(POINT point)
{
    const int top = chromeHeight();
    if (point.y < top) {
        updateHover(point);
        return;
    }

    clearHover();

    RECT client { };
    GetClientRect(m_window, &client);
    const POINT local { point.x, point.y - top };
    if (m_overview)
        overviewMove(local, client.right);
    else if (sidebarVisibleNow())
        sidebarMove(local, std::max(0, static_cast<int>(client.bottom) - top));
}

void Browser::onMouseLeave()
{
    clearHover();

    if (m_overviewHoverCard != -1 || m_sidebarHoverRow != -1) {
        m_overviewHoverCard = -1;
        m_overviewHoverClose = false;
        m_sidebarHoverRow = -1;
        m_sidebarHoverClose = false;
        InvalidateRect(m_window, nullptr, FALSE);
    }
}

void Browser::onNcMouseMove()
{
    clearHover();
}

bool Browser::onKeyDown(WPARAM key)
{
    if (m_overview && key == VK_ESCAPE) {
        hideOverview();
        return true;
    }
    return false;
}

bool Browser::onLeftButtonDown(POINT point)
{
    const ChromeLayout l = layout();

    // Below the toolbar: only reached when the main window itself is clicked
    // (the sidebar or the tab overview), not a page.
    if (point.y >= l.totalHeight) {
        if (m_downloadsVisible)
            hideDownloadsPanel(false);
        if (m_editing) {
            SetFocus(m_window);
            endAddressEdit();
        }

        RECT client { };
        GetClientRect(m_window, &client);
        const int height = std::max(0, static_cast<int>(client.bottom - l.totalHeight));
        const POINT local { point.x, point.y - l.totalHeight };

        if (m_overview)
            return overviewClick(local, client.right, height);
        if (sidebarVisibleNow())
            return sidebarClick(local, height);
        return false;
    }

    const HitResult hit = hitTest(point, l);

    // Remember which control is being held, so the paint code can press it in
    // and the release can spring it back out.
    m_pressTarget = static_cast<int>(hit.target);
    syncAnimations();

    if (m_downloadsVisible && hit.target != Target::Downloads)
        hideDownloadsPanel(false);

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
        showShareMenu();
        return true;
    case Target::NewTab:
        onCommand(kCmdNewTab);
        return true;
    case Target::TabOverview:
        onCommand(kCmdTabOverview);
        return true;
    case Target::Tab:
        activateTab(hit.tab);
        return true;
    case Target::TabClose:
        closeTab(hit.tab);
        return true;
    case Target::Downloads:
        toggleDownloadsPanel();
        return true;
    case Target::FindPrevious:
        runFind(true, false);
        return true;
    case Target::FindNext:
        runFind(false, false);
        return true;
    case Target::FindDone:
        hideFind();
        return true;
    }

    return false;
}

void Browser::onLeftButtonUp()
{
    if (m_pressTarget < 0)
        return;

    m_pressTarget = -1;
    syncAnimations();
}

LRESULT Browser::onNcHitTest(LRESULT defaultHit, POINT screenPoint)
{
    POINT point = screenPoint;
    ScreenToClient(m_window, &point);

    RECT client { };
    GetClientRect(m_window, &client);

    // Maximized: the system frame is still the real one, so let its answer
    // through. That keeps drag-to-restore and the snap layout flyout working.
    if (IsZoomed(m_window) && defaultHit != HTCLIENT)
        return defaultHit;

    // Restored: the window is frameless, so the resize border that used to
    // live in the non-client area is now inside our client rect and Windows
    // reports HTCLIENT for all of it. Recreate the border by hand, otherwise
    // the window could only ever be resized from the top edge.
    const int border = px(6);
    const bool onLeft = point.x < border;
    const bool onRight = point.x >= client.right - border;
    const bool onTop = point.y < border;
    const bool onBottom = point.y >= client.bottom - border;

    if (onTop) {
        if (onLeft)
            return HTTOPLEFT;
        if (onRight)
            return HTTOPRIGHT;
        return HTTOP;
    }
    if (onBottom) {
        if (onLeft)
            return HTBOTTOMLEFT;
        if (onRight)
            return HTBOTTOMRIGHT;
        return HTBOTTOM;
    }
    if (onLeft)
        return HTLEFT;
    if (onRight)
        return HTRIGHT;

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
    // If the main window receives the wheel (focus was not on the page, or the
    // cursor is over the sidebar or the tab overview), handle it here;
    // otherwise hand it to the page. The guard stops a forwarding loop, since
    // an unhandled wheel message is passed back up to this window.
    if (m_forwardingWheel)
        return false;

    POINT point { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    ScreenToClient(m_window, &point);

    RECT client { };
    GetClientRect(m_window, &client);
    const int top = chromeHeight();
    const int height = std::max(0, static_cast<int>(client.bottom) - top);
    const int step = MulDiv(GET_WHEEL_DELTA_WPARAM(wParam), px(60), 120);

    if (point.y >= top) {
        if (m_overview) {
            m_overviewScroll = std::clamp(m_overviewScroll - step, 0, overviewMaxScroll(client.right, height));
            InvalidateRect(m_window, nullptr, FALSE);
            return true;
        }
        if (sidebarVisibleNow() && point.x < px(kSidebarWidth)) {
            m_sidebarScroll = std::clamp(m_sidebarScroll - step, 0, sidebarMaxScroll(height));
            InvalidateRect(m_window, nullptr, FALSE);
            return true;
        }
    }

    auto* view = active();
    HWND target = view ? view->hwnd() : nullptr;
    if (!target || m_overview)
        return false;

    m_forwardingWheel = true;
    SendMessageW(target, WM_MOUSEWHEEL, wParam, lParam);
    m_forwardingWheel = false;
    return true;
}

LRESULT Browser::onCtlColorEdit(HDC dc, HWND control)
{
    if (control == m_find && m_find) {
        SetTextColor(dc, m_theme.text);
        SetBkColor(dc, m_theme.pill);
        return reinterpret_cast<LRESULT>(m_findBrush);
    }

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
    const int width = client.right;
    const int contentHeight = std::max(0, static_cast<int>(client.bottom - l.totalHeight));
    const RECT content { 0, l.totalHeight, width, l.totalHeight + contentHeight };

    // Paint only what was actually invalidated. WM_PAINT arrives with an
    // update region, and using it means a twenty-pixel sidebar hover repaints
    // twenty pixels instead of clearing and redrawing a hundred and eighty
    // square centimetres of chrome - which is both the lag and, because the
    // page fill gets rebuilt from scratch, the flicker.
    RECT update { };
    if (!GetUpdateRect(m_window, &update, FALSE))
        update = client;
    else
        IntersectRect(&update, &update, &client);

    const bool overviewUp = m_overview || m_overviewAnim.value > 0.0;
    const int sidebarWidth = sidebarWidthNow();

    // The page fill is drawn straight to the DC - it is a single opaque
    // rectangle - so it only runs when it is inside the update region.
    if (update.top < content.bottom) {
        RECT pageArea { std::max<LONG>(update.left, 0), content.top,
            std::min<LONG>(update.right, width), content.bottom };
        if (pageArea.right > pageArea.left && pageArea.bottom > pageArea.top)
            fillSolid(target, pageArea, m_theme.page);
    }

    if (overviewUp) {
        if (update.top < content.bottom) {
            RECT area = content;
            IntersectRect(&area, &area, &update);
            paintBuffered(target, area, [&](HDC dc) { paintOverview(dc, width, contentHeight); });
        }
    } else if (sidebarWidth > 0 && update.left < sidebarWidth && update.top < content.bottom) {
        const RECT panel { 0, l.totalHeight, sidebarWidth, l.totalHeight + contentHeight };
        RECT area = panel;
        IntersectRect(&area, &area, &update);
        paintBuffered(target, area, [&](HDC dc) { paintSidebar(dc, sidebarWidth, contentHeight); });
    }

    // The chrome is drawn into an off-screen bitmap to avoid flicker.
    if (update.top < l.totalHeight) {
        const RECT chrome { 0, 0, width, l.totalHeight };
        RECT area = chrome;
        IntersectRect(&area, &area, &update);
        paintBuffered(target, area, [&](HDC dc) { paintChrome(dc, l); });
    }
}

void Browser::releaseBackBuffer()
{
    if (m_backDC) {
        if (m_backPrevious) {
            SelectObject(m_backDC, m_backPrevious);
            m_backPrevious = nullptr;
        }
        DeleteDC(m_backDC);
        m_backDC = nullptr;
    }
    if (m_backBitmap) {
        DeleteObject(m_backBitmap);
        m_backBitmap = nullptr;
    }
    m_backWidth = 0;
    m_backHeight = 0;
}

void Browser::paintBuffered(HDC target, const RECT& area, const std::function<void(HDC)>& draw)
{
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    if (width <= 0 || height <= 0)
        return;

    // The buffer shares the client's origin, so it has to reach to the far
    // edge of the area even when only a corner of it is being painted.
    // area.right and area.bottom are LONG, so the zero has to be LONG as well -
    // std::max cannot deduce a single type across the two.
    const LONG neededWidth = std::max<LONG>(area.right, 0);
    const LONG neededHeight = std::max<LONG>(area.bottom, 0);

    // Drawing the chrome into a throwaway bitmap and blitting the result was
    // costing more than the chrome itself: a window-sized bitmap is around
    // eight megabytes, and an animation produces dozens of paints a second.
    // Keep one and grow it only when the window outgrows it.
    if (!m_backDC || neededWidth > m_backWidth || neededHeight > m_backHeight) {
        releaseBackBuffer();
        m_backDC = CreateCompatibleDC(target);
        m_backBitmap = CreateCompatibleBitmap(target,
            static_cast<int>(neededWidth), static_cast<int>(neededHeight));
        if (!m_backDC || !m_backBitmap) {
            releaseBackBuffer();
            return;
        }
        m_backWidth = neededWidth;
        m_backHeight = neededHeight;
        m_backPrevious = SelectObject(m_backDC, m_backBitmap);
    }

    // Confine the drawing to the damaged slice, and blit from the same place
    // in the buffer rather than from its corner - otherwise a partial repaint
    // would put the wrong pixels on screen.
    //
    // The draw callbacks paint in their *own* coordinates, with (0,0) at the
    // top-left of whatever they own. The chrome starts at the top of the
    // client so its origin is already (0,0), but the sidebar and the overview
    // start below the chrome and paint from (0,0) too. The buffer, on the
    // other hand, shares the client's origin, so for those two the origin has
    // to be moved onto the area's corner first. Without this the top
    // chromeHeight() pixels of the sidebar and the overview fall outside the
    // clip region and are thrown away, and the blit reads rows that were
    // never drawn - which is what collapsed the tab overview into a thin
    // strip along the top of an otherwise black window.
    POINT previousOrigin { };
    SetViewportOrgEx(m_backDC, area.left, area.top, &previousOrigin);

    HRGN clip = CreateRectRgnIndirect(&area);
    SelectClipRgn(m_backDC, clip);
    DeleteObject(clip);

    draw(m_backDC);

    SetViewportOrgEx(m_backDC, previousOrigin.x, previousOrigin.y, nullptr);

    BitBlt(target, area.left, area.top, width, height,
        m_backDC, area.left, area.top, SRCCOPY);
}

void Browser::drawLabel(HDC dc, Gdiplus::Graphics& g, const std::wstring& text, RECT rect, COLORREF color, UINT flags) const
{
    g.Flush(Gdiplus::FlushIntentionSync);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, m_font));
    DrawTextW(dc, text.c_str(), -1, &rect, flags);
    SelectObject(dc, oldFont);
}

int Browser::measureLabel(HDC dc, Gdiplus::Graphics& g, const std::wstring& text) const
{
    g.Flush(Gdiplus::FlushIntentionSync);
    RECT rect { 0, 0, 0, 0 };
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, m_font));
    DrawTextW(dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);
    return static_cast<int>(rect.right - rect.left);
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
    if (l.findBarHeight > 0)
        fillSolid(dc, l.findBar, m_theme.toolbar);
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

    // Linear colour blend, used to fade the pressed state in.
    auto mix = [](COLORREF from, COLORREF to, double t) {
        const auto channel = [t](int a, int b) {
            return static_cast<BYTE>(std::lround(a + (b - a) * t));
        };
        return RGB(channel(GetRValue(from), GetRValue(to)),
            channel(GetGValue(from), GetGValue(to)),
            channel(GetBValue(from), GetBValue(to)));
    };

    // A control that is being held darkens a step further than a plain hover,
    // and springs back out when the button is released. This is the short,
    // snappy spring (0.15 / 0.86) rather than the layout one.
    auto drawPress = [&](Target which, const RECT& rect) {
        if (m_pressTarget != static_cast<int>(which))
            return;

        RECT inset = rect;
        InflateRect(&inset, -px(2), -px(2));
        const double t = std::clamp(m_pressAnim.value, 0.0, 1.0);
        const COLORREF deep = m_theme.dark ? RGB(88, 88, 94) : RGB(212, 212, 218);
        fillRoundedRect(g, inset, (inset.bottom - inset.top) / 2, mix(m_theme.glassHover, deep, t));
    };

    drawGlass(l.sidebar);
    if (m_hover.target == Target::Sidebar)
        drawHot(l.sidebar);
    drawPress(Target::Sidebar, l.sidebar);
    drawSidebarIcon(g, l.sidebar, m_theme.text, k);

    drawGlass(l.navGroup);
    if (m_hover.target == Target::Back && canGoBack)
        drawHot(l.back);
    if (m_hover.target == Target::Forward && canGoForward)
        drawHot(l.forward);
    drawPress(Target::Back, l.back);
    drawPress(Target::Forward, l.forward);
    drawChevronIcon(g, l.back, false, canGoBack ? m_theme.text : m_theme.textDisabled, k);
    drawChevronIcon(g, l.forward, true, canGoForward ? m_theme.text : m_theme.textDisabled, k);

    drawGlass(l.rightGroup);
    if (l.hasDownloads) {
        if (m_hover.target == Target::Downloads)
            drawHot(l.downloadsButton);
        drawPress(Target::Downloads, l.downloadsButton);
        const Downloads& downloads = Downloads::instance();
        drawDownloadsIcon(g, l.downloadsButton, m_theme.text, m_theme.accent, downloads.hasActive(), downloads.progress(), k);
    }
    if (m_hover.target == Target::Share)
        drawHot(l.shareButton);
    if (m_hover.target == Target::NewTab)
        drawHot(l.newTabButton);
    if (m_hover.target == Target::TabOverview)
        drawHot(l.overviewButton);
    drawPress(Target::Share, l.shareButton);
    drawPress(Target::NewTab, l.newTabButton);
    drawPress(Target::TabOverview, l.overviewButton);
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
        // Safari shows load progress as a thin line along the bottom edge of
        // the address field, clipped to the field's rounded shape.
        Gdiplus::GraphicsPath clipPath;
        addRoundedRect(clipPath, toRectF(l.pill), static_cast<Gdiplus::REAL>(pillRadius));
        g.SetClip(&clipPath);

        const int lineHeight = std::max(2, px(2));
        const double shown = std::clamp(progress, 0.06, 1.0);
        const RECT bar {
            l.pill.left,
            l.pill.bottom - lineHeight,
            l.pill.left + static_cast<LONG>((l.pill.right - l.pill.left) * shown),
            l.pill.bottom
        };
        Gdiplus::SolidBrush progressBrush(toGdiColor(m_theme.accent));
        g.FillRectangle(&progressBrush, toRectF(bar));
        g.ResetClip();
    }

    if (m_editing) {
        // The focus ring fades up rather than appearing at full strength the
        // instant the field takes focus.
        const double t = std::clamp(m_addressAnim.value, 0.0, 1.0);
        strokeRoundedRect(g, l.pill, pillRadius, m_theme.accent, 2.0f * k,
            static_cast<BYTE>(210.0 * t + 0.5));
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

    // --- find bar ---
    if (l.findBarHeight > 0) {
        const int fieldRadius = (l.findField.bottom - l.findField.top) / 2;
        fillRoundedRect(g, l.findField, fieldRadius, m_theme.pill);

        const int matches = current ? current->findMatches() : -1;
        const bool canStep = matches > 0;

        auto stepHot = [&](const RECT& rect, Target target) {
            if (m_hover.target == target && canStep) {
                RECT inset = rect;
                InflateRect(&inset, -px(2), -px(2));
                fillRoundedRect(g, inset, px(8), m_theme.pill);
            }
        };
        stepHot(l.findPrevious, Target::FindPrevious);
        stepHot(l.findNext, Target::FindNext);
        drawVerticalChevron(g, l.findPrevious, true, canStep ? m_theme.text : m_theme.textDisabled, k);
        drawVerticalChevron(g, l.findNext, false, canStep ? m_theme.text : m_theme.textDisabled, k);

        std::wstring status;
        if (matches == 0)
            status = L"Not found";
        else if (matches == 1)
            status = L"1 match";
        else if (matches > 1)
            status = std::to_wstring(matches) + L" matches";
        drawText(status, l.findStatus, m_theme.textSecondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        if (m_hover.target == Target::FindDone)
            fillRoundedRect(g, l.findDone, px(8), m_theme.pill);
        drawText(L"Done", l.findDone, m_theme.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    g.Flush(Gdiplus::FlushIntentionSync);
}

// ---------------------------------------------------------------------------
// Library actions
// ---------------------------------------------------------------------------

RECT Browser::contentArea() const
{
    RECT client { };
    GetClientRect(m_window, &client);
    client.top = chromeHeight();
    return client;
}

void Browser::bookmarkCurrent()
{
    auto* view = active();
    if (!view || isStartPage(view->url()))
        return;

    m_library.addBookmark(view->url(), tabTitle(*view));

    // Show the result, like Safari opening the sidebar on Bookmarks. The
    // width has to be synced before anything reads it - sidebarWidthNow()
    // reports the spring's settled target, so a sidebar that was closed would
    // stay closed no matter what m_sidebarVisible says.
    m_sidebarVisible = true;
    m_sidebarMode = 0;
    m_sidebarScroll = 0;
    if (m_overview)
        hideOverview();
    syncAnimations();
    invalidateSidebarColumn();
}

void Browser::readingListCurrent()
{
    auto* view = active();
    if (!view || isStartPage(view->url()))
        return;

    m_library.addToReadingList(view->url(), tabTitle(*view));

    m_sidebarVisible = true;
    m_sidebarMode = 1;
    m_sidebarScroll = 0;
    if (m_overview)
        hideOverview();
    syncAnimations();
    invalidateSidebarColumn();
}

// ---------------------------------------------------------------------------
// Tab overview
// ---------------------------------------------------------------------------

void Browser::captureThumbnail(WebView& view)
{
    HWND window = view.hwnd();
    if (!window || !IsWindowVisible(window) || IsIconic(m_window))
        return;

    RECT rect { };
    if (!GetWindowRect(window, &rect))
        return;

    const int sourceWidth = static_cast<int>(rect.right - rect.left);
    const int sourceHeight = static_cast<int>(rect.bottom - rect.top);
    if (sourceWidth <= 0 || sourceHeight <= 0)
        return;

    const int thumbWidth = 480;
    const int thumbHeight = std::max(1, sourceHeight * thumbWidth / sourceWidth);

    // Copy what is on screen: the page is composited by the graphics system,
    // so reading the window's own surface would not work reliably.
    HDC screen = GetDC(nullptr);
    if (!screen)
        return;

    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, thumbWidth, thumbHeight);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    SetStretchBltMode(memory, HALFTONE);
    SetBrushOrgEx(memory, 0, 0, nullptr);
    const BOOL copied = StretchBlt(
        memory, 0, 0, thumbWidth, thumbHeight,
        screen, rect.left, rect.top, sourceWidth, sourceHeight,
        SRCCOPY | CAPTUREBLT);
    SelectObject(memory, previous);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    if (copied)
        view.setThumbnail(bitmap);
    else
        DeleteObject(bitmap);
}

void Browser::showOverview()
{
    if (m_overview) {
        hideOverview();
        return;
    }

    endAddressEdit();

    if (auto* view = active())
        captureThumbnail(*view);

    m_overview = true;
    m_overviewScroll = 0;
    m_overviewHoverCard = -1;
    m_overviewHoverClose = false;

    for (const auto& tab : m_tabs)
        tab->setVisible(false);

    syncAnimations();
    SetFocus(m_window);
    InvalidateRect(m_window, nullptr, FALSE);
}

void Browser::hideOverview()
{
    if (!m_overview)
        return;

    m_overview = false;
    m_overviewHoverCard = -1;
    m_overviewHoverClose = false;

    // The tab views stay hidden until the overlay has finished easing out, so
    // the page is revealed by the fade instead of snapping in behind it.
    // stepAnimations() is what brings them back.
    syncAnimations();
    resize();
    if (auto* view = active())
        view->focus();
    InvalidateRect(m_window, nullptr, FALSE);
}

std::vector<Browser::OverviewCard> Browser::overviewLayout(int width) const
{
    std::vector<OverviewCard> cards;

    const int count = static_cast<int>(m_tabs.size());
    if (count == 0)
        return cards;

    const int cardWidth = px(260);
    const int thumbHeight = cardWidth * 10 / 16;
    const int titleHeight = px(34);
    const int gap = px(28);
    const int margin = px(48);
    const int top = px(40);

    const int columns = std::clamp((width - margin * 2 + gap) / (cardWidth + gap), 1, count);
    const int gridWidth = columns * cardWidth + (columns - 1) * gap;
    const int left = (width - gridWidth) / 2;

    for (int i = 0; i < count; ++i) {
        const int row = i / columns;
        const int column = i % columns;
        const int x = left + column * (cardWidth + gap);
        const int y = top + row * (thumbHeight + titleHeight + gap) - m_overviewScroll;

        OverviewCard card;
        card.thumb = { x, y, x + cardWidth, y + thumbHeight };
        card.card = { x, y, x + cardWidth, y + thumbHeight + titleHeight };
        card.close = { x + px(8), y + px(8), x + px(8) + px(24), y + px(8) + px(24) };
        card.title = { x, y + thumbHeight + px(6), x + cardWidth, y + thumbHeight + titleHeight };
        cards.push_back(card);
    }

    return cards;
}

int Browser::overviewMaxScroll(int width, int height) const
{
    const int count = static_cast<int>(m_tabs.size());
    if (count == 0)
        return 0;

    const int cardWidth = px(260);
    const int rowHeight = cardWidth * 10 / 16 + px(34) + px(28);
    const int columns = std::clamp((width - px(48) * 2 + px(28)) / (cardWidth + px(28)), 1, count);
    const int rows = (count + columns - 1) / columns;
    const int total = px(40) + rows * rowHeight + px(20);
    return std::max(0, total - height);
}

bool Browser::overviewClick(POINT local, int width, int height)
{
    (void)height;
    const std::vector<OverviewCard> cards = overviewLayout(width);

    for (size_t i = 0; i < cards.size(); ++i) {
        if (PtInRect(&cards[i].close, local)) {
            closeTab(i);
            return true;
        }
        if (PtInRect(&cards[i].card, local)) {
            m_overview = false;
            m_overviewHoverCard = -1;
            m_overviewHoverClose = false;
            activateTab(i);
            InvalidateRect(m_window, nullptr, FALSE);
            return true;
        }
    }

    // A click on the empty background returns to the page.
    hideOverview();
    return true;
}

void Browser::overviewMove(POINT local, int width)
{
    const std::vector<OverviewCard> cards = overviewLayout(width);

    int hoverCard = -1;
    bool hoverClose = false;
    for (size_t i = 0; i < cards.size(); ++i) {
        if (PtInRect(&cards[i].card, local)) {
            hoverCard = static_cast<int>(i);
            hoverClose = PtInRect(&cards[i].close, local) != FALSE;
            break;
        }
    }

    if (hoverCard == m_overviewHoverCard && hoverClose == m_overviewHoverClose)
        return;

    m_overviewHoverCard = hoverCard;
    m_overviewHoverClose = hoverClose;
    InvalidateRect(m_window, nullptr, FALSE);
}

void Browser::releaseOverviewSurface()
{
    delete m_overviewSurface;
    m_overviewSurface = nullptr;
}

void Browser::paintOverview(HDC dc, int width, int height)
{
    // The overview eases in and out, so it is drawn offscreen and composited
    // with an alpha rather than being painted straight onto the chrome. Two
    // things happen at once: the whole grid fades, and the cards grow from
    // 94% to full size about the centre of the content area.
    const double t = std::clamp(m_overviewAnim.value, 0.0, 1.0);
    if (t <= 0.002 || width <= 0 || height <= 0)
        return;

    // Compositing needs a scratch surface the size of the content area, and a
    // 1920-wide one at four bytes a pixel is about seven megabytes. Building
    // one per paint threw away that much memory a dozen times a second for the
    // whole time the grid was animating, which is a lot of allocator churn on
    // the thread that also has to answer clicks. Keep one and grow it only when
    // the window outgrows it; paintOverviewContent fills the whole surface
    // before it draws anything, so nothing stale can survive.
    if (!m_overviewSurface
        || m_overviewSurface->GetWidth() < width
        || m_overviewSurface->GetHeight() < height) {
        releaseOverviewSurface();
        m_overviewSurface = new Gdiplus::Bitmap(width, height, PixelFormat32bppPARGB);
        log(L"Overview surface resized.");
    }
    if (!m_overviewSurface)
        return;

    {
        Gdiplus::Graphics scratch(m_overviewSurface);
        scratch.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        HDC bufferDc = scratch.GetHDC();
        paintOverviewContent(bufferDc, width, height);
        scratch.ReleaseHDC(bufferDc);
    }

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

    Gdiplus::ImageAttributes attributes;
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    // Gdiplus::ColorMatrix is a plain REAL[5][5]; fill it by hand rather than
    // calling a method that does not exist in this SDK.
    Gdiplus::ColorMatrix matrix { };
    matrix.m[0][0] = 1.0f;
    matrix.m[1][1] = 1.0f;
    matrix.m[2][2] = 1.0f;
    matrix.m[3][3] = static_cast<Gdiplus::REAL>(t);
    matrix.m[4][4] = 1.0f;
    attributes.SetColorMatrix(&matrix);

    const float scale = 0.94f + 0.06f * static_cast<float>(t);

    Gdiplus::RectF destRect(
        static_cast<Gdiplus::REAL>(width * (1.0 - scale) * 0.5),
        static_cast<Gdiplus::REAL>(height * (1.0 - scale) * 0.5),
        static_cast<Gdiplus::REAL>(width * scale),
        static_cast<Gdiplus::REAL>(height * scale));
    g.DrawImage(m_overviewSurface, destRect,
        0.0f, 0.0f,
        static_cast<Gdiplus::REAL>(width), static_cast<Gdiplus::REAL>(height),
        Gdiplus::UnitPixel, &attributes);
}

void Browser::paintOverviewContent(HDC dc, int width, int height)
{
    const RECT whole { 0, 0, width, height };
    fillSolid(dc, whole, m_theme.dark ? RGB(22, 22, 24) : RGB(226, 226, 232));

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    const float k = scaleFactor();
    const int radius = px(10);
    const std::vector<OverviewCard> cards = overviewLayout(width);

    for (size_t i = 0; i < cards.size(); ++i) {
        const OverviewCard& card = cards[i];
        if (card.card.bottom < 0 || card.card.top > height)
            continue;

        const WebView& view = *m_tabs[i];
        const bool isActive = i == m_active;
        const bool hot = static_cast<int>(i) == m_overviewHoverCard;

        // Picture of the page.
        fillRoundedRect(g, card.thumb, radius, m_theme.page);
        if (HBITMAP bitmap = view.thumbnail()) {
            Gdiplus::GraphicsPath path;
            addRoundedRect(path, toRectF(card.thumb), static_cast<Gdiplus::REAL>(radius));
            g.SetClip(&path);
            Gdiplus::Bitmap image(bitmap, nullptr);
            g.DrawImage(&image, toRectF(card.thumb));
            g.ResetClip();
        } else if (isStartPage(view.url())) {
            drawStartPageIcon(g, card.thumb, m_theme.textSecondary, k * 3.0f);
        }

        strokeRoundedRect(
            g, card.thumb, radius,
            isActive ? m_theme.accent : m_theme.tabBorder,
            isActive ? 3.0f * k : std::max(1.0f, k));

        // Close button, shown on hover (and always for the active tab).
        if (hot || isActive) {
            fillRoundedRect(g, card.close, (card.close.bottom - card.close.top) / 2,
                (hot && m_overviewHoverClose) ? m_theme.glassHover : m_theme.glass);
            strokeRoundedRect(g, card.close, (card.close.bottom - card.close.top) / 2, m_theme.glassBorder, std::max(1.0f, k));
            drawCloseIcon(g, card.close, m_theme.text, k);
        }

        // Title with the site badge, centered under the picture.
        const std::wstring title = tabTitle(view);
        const int iconSize = px(14);
        const int textWidth = std::min(measureLabel(dc, g, title), std::max(0, static_cast<int>(card.title.right - card.title.left) - iconSize - px(6)));
        const int groupWidth = iconSize + px(6) + textWidth;
        const int groupLeft = card.title.left + ((card.title.right - card.title.left) - groupWidth) / 2;
        const int centerY = (card.title.top + card.title.bottom) / 2;
        const RECT iconRect { groupLeft, centerY - iconSize / 2, groupLeft + iconSize, centerY - iconSize / 2 + iconSize };

        if (isStartPage(view.url())) {
            drawStartPageIcon(g, iconRect, m_theme.textSecondary, k);
        } else {
            const std::wstring host = hostOf(view.url());
            drawSiteBadge(g, iconRect, badgeLetter(host), colorFromHue(hueOf(host)), k);
        }

        const RECT textRect { iconRect.right + px(6), card.title.top, iconRect.right + px(6) + textWidth, card.title.bottom };
        drawLabel(dc, g, title, textRect, m_theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    g.Flush(Gdiplus::FlushIntentionSync);
}

// ---------------------------------------------------------------------------
// Sidebar
// ---------------------------------------------------------------------------

const std::vector<LibraryEntry>& Browser::sidebarEntries() const
{
    if (m_sidebarMode == 0)
        return m_library.bookmarks();
    if (m_sidebarMode == 1)
        return m_library.readingList();
    return m_library.history();
}

Browser::SidebarLayout Browser::sidebarLayout(int height) const
{
    SidebarLayout s;
    s.width = px(kSidebarWidth);
    s.height = height;
    s.rowHeight = px(46);

    const int margin = px(12);
    s.segmented = { margin, margin, s.width - margin, margin + px(30) };

    const int segmentWidth = (s.segmented.right - s.segmented.left) / 3;
    for (int i = 0; i < 3; ++i) {
        const int left = s.segmented.left + i * segmentWidth;
        const int right = i == 2 ? s.segmented.right : left + segmentWidth;
        s.segments[i] = { left, s.segmented.top, right, s.segmented.bottom };
    }

    // The footer holds the always-visible gear (right at the bottom) and, in
    // History mode, the "Clear History" button stacked above it. Each gets its
    // own band so neither one bleeds onto the row list nor onto each other.
    const int listTop = s.segmented.bottom + margin;
    const int gearSize = px(32);
    const int gearBand = margin + gearSize;        // bottom band
    const int clearHeight = px(28);
    const int clearGap = px(18);                   // visible breathing room above the gear

    // Gear always sits in its own band at the very bottom of the sidebar.
    const int gearTop = std::max(listTop, height - gearBand);
    const int gearBottom = height - margin;

    int listBottom = gearTop - margin;
    int clearTop = 0, clearBottom = 0;
    if (m_sidebarMode == 2) {
        clearBottom = std::max(listTop + clearHeight, gearTop - clearGap);
        clearTop = clearBottom - clearHeight;
        listBottom = clearTop - margin;
    }
    s.list = { 0, listTop, s.width, std::max(listTop, listBottom) };

    if (m_sidebarMode == 2) {
        // Clear sits directly above the gear band with a real gap between them.
        s.clearButton = {
            margin,
            clearTop,
            s.width - margin,
            clearBottom };
    }

    // Gear at the very bottom-left, with the same left margin as the segmented
    // control so the eye reads it as part of the sidebar column.
    s.gearButton = {
        margin,
        gearTop,
        margin + gearSize,
        gearBottom };
    return s;
}

RECT Browser::sidebarRow(const SidebarLayout& layout, size_t index) const
{
    const int top = layout.list.top + static_cast<int>(index) * layout.rowHeight - m_sidebarScroll;
    return { 0, top, layout.width, top + layout.rowHeight };
}

RECT Browser::sidebarRowClose(const RECT& row) const
{
    const int size = px(20);
    const int right = row.right - px(14);
    const int top = row.top + ((row.bottom - row.top) - size) / 2;
    return { right - size, top, right, top + size };
}

int Browser::sidebarMaxScroll(int height) const
{
    const SidebarLayout s = sidebarLayout(height);
    const int total = static_cast<int>(sidebarEntries().size()) * s.rowHeight;
    return std::max(0, total - static_cast<int>(s.list.bottom - s.list.top));
}

bool Browser::sidebarVisibleNow() const
{
    if (!m_sidebarVisible)
        return false;
    // The built-in pages are meant to read as full browser surfaces rather
    // than as web content sitting next to a sidebar.
    return m_active != m_settingsPageTab && m_active != m_downloadsPageTab;
}

void Browser::toggleSidebar()
{
    m_sidebarVisible = !m_sidebarVisible;
    m_sidebarScroll = 0;
    m_sidebarHoverRow = -1;
    m_sidebarHoverClose = false;

    if (m_overview)
        hideOverview();

    // Let the width spring to its new value rather than jumping the page
    // sideways; resize() reads the interpolated width every frame. The first
    // animation tick does the real repaint - invalidating the whole window
    // here as well would just clear the page fill behind the web view for
    // nothing, which is the flash at the start of every toggle.
    syncAnimations();
    invalidateSidebarColumn();
}

bool Browser::sidebarClick(POINT local, int height)
{
    const SidebarLayout s = sidebarLayout(height);
    if (local.x < 0 || local.x >= s.width)
        return false;

    for (int i = 0; i < 3; ++i) {
        if (PtInRect(&s.segments[i], local)) {
            m_sidebarMode = i;
            m_sidebarScroll = 0;
            m_sidebarHoverRow = -1;
            InvalidateRect(m_window, nullptr, FALSE);
            return true;
        }
    }

    if (m_sidebarMode == 2 && PtInRect(&s.clearButton, local)) {
        m_library.clearHistory();
        m_sidebarScroll = 0;
        InvalidateRect(m_window, nullptr, FALSE);
        return true;
    }

    // Settings (gear) is bottom-left and floats over the list; check it
    // before iterating list rows so a click on the gear always wins.
    if (PtInRect(&s.gearButton, local)) {
        openSettings();
        return true;
    }

    if (PtInRect(&s.list, local)) {
        const std::vector<LibraryEntry>& entries = sidebarEntries();
        for (size_t i = 0; i < entries.size(); ++i) {
            const RECT row = sidebarRow(s, i);
            if (!PtInRect(&row, local))
                continue;

            const RECT closeRect = sidebarRowClose(row);
            if (PtInRect(&closeRect, local)) {
                if (m_sidebarMode == 0)
                    m_library.removeBookmark(i);
                else if (m_sidebarMode == 1)
                    m_library.removeFromReadingList(i);
                else
                    m_library.removeFromHistory(i);

                m_sidebarScroll = std::clamp(m_sidebarScroll, 0, sidebarMaxScroll(height));
                InvalidateRect(m_window, nullptr, FALSE);
                return true;
            }

            const std::wstring url = entries[i].url;
            if (auto* view = active())
                view->load(url);
            return true;
        }
    }

    return true;
}

void Browser::sidebarMove(POINT local, int height)
{
    const SidebarLayout s = sidebarLayout(height);

    int hoverRow = -1;
    bool hoverClose = false;
    if (local.x >= 0 && local.x < s.width && PtInRect(&s.list, local)) {
        const std::vector<LibraryEntry>& entries = sidebarEntries();
        for (size_t i = 0; i < entries.size(); ++i) {
            const RECT row = sidebarRow(s, i);
            if (!PtInRect(&row, local))
                continue;

            hoverRow = static_cast<int>(i);
            const RECT closeRect = sidebarRowClose(row);
            hoverClose = PtInRect(&closeRect, local) != FALSE;
            break;
        }
    }

    if (hoverRow == m_sidebarHoverRow && hoverClose == m_sidebarHoverClose)
        return;

    // The previous hover row needs to lose its highlight, the new row needs
    // to gain one. The actual updated band is at most two row-high slices of
    // the list plus the footer; repainting the whole window clears the page
    // area behind the WebView and produces a flicker every time the cursor
    // crosses a row, so we invalidate the sidebar bounds only.
    int heightPx = 0;
    {
        RECT client { };
        GetClientRect(m_window, &client);
        heightPx = std::max(0, static_cast<int>(client.bottom));
    }
    const int chromeH = chromeHeight();
    RECT sidebarArea { 0, chromeH, s.width, heightPx };
    InvalidateRect(m_window, &sidebarArea, FALSE);

    m_sidebarHoverRow = hoverRow;
    m_sidebarHoverClose = hoverClose;
}

void Browser::paintSidebar(HDC dc, int width, int height)
{
    const RECT whole { 0, 0, width, height };
    fillSolid(dc, whole, m_theme.tabBar);

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    const float k = scaleFactor();
    const SidebarLayout s = sidebarLayout(height);
    const std::vector<LibraryEntry>& entries = sidebarEntries();

    // Rows first; the header and footer are painted over them afterwards, so
    // rows scrolled past the edges are hidden without clipping.
    for (size_t i = 0; i < entries.size(); ++i) {
        const RECT row = sidebarRow(s, i);
        if (row.bottom < s.list.top || row.top > s.list.bottom)
            continue;

        const bool hot = static_cast<int>(i) == m_sidebarHoverRow;
        if (hot) {
            RECT inset = row;
            InflateRect(&inset, -px(8), -px(2));
            fillRoundedRect(g, inset, px(8), m_theme.pill);
        }

        const std::wstring host = hostOf(entries[i].url);
        const int badge = px(18);
        const RECT badgeRect { row.left + px(18), row.top + (s.rowHeight - badge) / 2, row.left + px(18) + badge, row.top + (s.rowHeight - badge) / 2 + badge };
        drawSiteBadge(g, badgeRect, badgeLetter(host), colorFromHue(hueOf(host)), k);

        const int textLeft = badgeRect.right + px(10);
        const int textRight = row.right - px(44);
        const std::wstring title = entries[i].title.empty() ? host : entries[i].title;
        const RECT titleRect { textLeft, row.top + px(5), textRight, row.top + px(5) + px(19) };
        const RECT hostRect { textLeft, row.top + px(5) + px(19), textRight, row.top + px(5) + px(19) + px(17) };
        drawLabel(dc, g, title, titleRect, m_theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        drawLabel(dc, g, host, hostRect, m_theme.textSecondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        if (hot) {
            const RECT closeRect = sidebarRowClose(row);
            if (m_sidebarHoverClose)
                fillRoundedRect(g, closeRect, (closeRect.bottom - closeRect.top) / 2, m_theme.glassHover);
            drawCloseIcon(g, closeRect, m_theme.textSecondary, k);
        }
    }

    if (entries.empty()) {
        static const wchar_t* const messages[3] = { L"No Bookmarks", L"No Reading List Items", L"No History" };
        RECT message { 0, s.list.top, width, s.list.top + px(60) };
        drawLabel(dc, g, messages[m_sidebarMode], message, m_theme.textSecondary, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // Header band with the segmented control.
    const RECT header { 0, 0, width, s.list.top };
    fillSolid(dc, header, m_theme.tabBar);

    fillRoundedRect(g, s.segmented, px(8), m_theme.pill);
    static const wchar_t* const names[3] = { L"Bookmarks", L"Reading List", L"History" };
    for (int i = 0; i < 3; ++i) {
        const bool selected = i == m_sidebarMode;
        if (selected) {
            RECT inset = s.segments[i];
            InflateRect(&inset, -px(2), -px(2));
            fillRoundedRect(g, inset, px(6), m_theme.glass);
            strokeRoundedRect(g, inset, px(6), m_theme.glassBorder, std::max(1.0f, k));
        }
        drawLabel(dc, g, names[i], s.segments[i], selected ? m_theme.text : m_theme.textSecondary,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // Footer with "Clear History" on the History tab.
    if (m_sidebarMode == 2) {
        const RECT footer { 0, s.list.bottom, width, height };
        fillSolid(dc, footer, m_theme.tabBar);
        fillRoundedRect(g, s.clearButton, px(8), m_theme.pill);
        drawLabel(dc, g, L"Clear History", s.clearButton, m_theme.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // Settings gear in the bottom-left corner, always visible. Filled with the
    // accent colour so it is the one unmistakable control in the sidebar.
    {
        RECT gearInset = s.gearButton;
        InflateRect(&gearInset, -px(2), -px(2));
        const int gearRadius = (gearInset.bottom - gearInset.top) / 2;
        fillRoundedRect(g, gearInset, std::max(2, gearRadius), m_theme.accent);
        drawGearIcon(g, gearInset, RGB(255, 255, 255), m_theme.accent, k * 0.78f);
    }

    fillSolid(dc, RECT { width - 1, 0, width, height }, m_theme.separator);
    g.Flush(Gdiplus::FlushIntentionSync);
}


// ---------------------------------------------------------------------------
// Pages opened by web content, and find in page
// ---------------------------------------------------------------------------

void Browser::attachCallbacks(WebView* view)
{
    view->onChanged = [this, view] { onViewChanged(view); };
    view->onNewPage = [this](WKPageConfigurationRef configuration) { return openPageFromWebKit(configuration); };
    view->onCloseRequested = [this, view] { closeTabForView(view); };
}

WKPageRef Browser::openPageFromWebKit(WKPageConfigurationRef configuration)
{
    // A link with target=_blank or window.open: show it as a new tab.
    m_overview = false;

    auto view = m_host.createView(m_window, configuration);
    if (!view)
        return nullptr;

    WebView* raw = view.get();
    attachCallbacks(raw);
    m_tabs.push_back(std::move(view));

    activateTab(m_tabs.size() - 1);
    InvalidateRect(m_window, nullptr, FALSE);
    return raw->page();
}

void Browser::closeTabForView(WebView* view)
{
    // window.close(). The last tab is never closed this way.
    if (m_tabs.size() <= 1)
        return;

    for (size_t i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i].get() == view) {
            closeTab(i);
            return;
        }
    }
}

void Browser::onControlNotify(HWND control, UINT code)
{
    if (control == m_find && code == EN_CHANGE)
        runFind(false, true);
}

void Browser::createFindControl()
{
    m_find = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        0, 0, 10, 10,
        m_window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCmdFindControl)),
        GetModuleHandleW(nullptr),
        nullptr);
    if (!m_find)
        return;

    SendMessageW(m_find, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    SendMessageW(m_find, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search on page"));

    SetWindowLongPtrW(m_find, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    m_findOriginal = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(m_find, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Browser::findProc)));
}

LRESULT CALLBACK Browser::findProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* browser = reinterpret_cast<Browser*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!browser || !browser->m_findOriginal)
        return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CHAR:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE)
            return 0;
        break;

    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            browser->runFind(GetKeyState(VK_SHIFT) < 0, false);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            browser->hideFind();
            return 0;
        }
        break;

    default:
        break;
    }

    return CallWindowProcW(browser->m_findOriginal, window, message, wParam, lParam);
}

void Browser::placeFindEdit(const ChromeLayout& l) const
{
    if (!m_find || l.findBarHeight <= 0)
        return;

    const int inset = px(12);
    const int height = px(20);
    const int top = l.findField.top + ((l.findField.bottom - l.findField.top) - height) / 2;
    const int width = std::max(px(40), static_cast<int>(l.findField.right - l.findField.left) - inset * 2);
    MoveWindow(m_find, l.findField.left + inset, top, width, height, TRUE);
}

void Browser::showFind()
{
    if (!m_find)
        return;

    if (m_overview)
        hideOverview();
    endAddressEdit();

    if (!m_findVisible) {
        m_findVisible = true;
        ShowWindow(m_find, SW_SHOW);
        syncAnimations();
        resize();
    }

    SetFocus(m_find);
    SendMessageW(m_find, EM_SETSEL, 0, -1);
    runFind(false, true);
    InvalidateRect(m_window, nullptr, FALSE);
}

void Browser::hideFind()
{
    if (!m_findVisible)
        return;

    m_findVisible = false;
    for (const auto& tab : m_tabs)
        tab->hideFind();

    // The edit control stays on screen until the slide-out lands; see
    // stepAnimations(), which hides it once the spring reaches zero.
    syncAnimations();
    resize();

    if (auto* view = active())
        view->focus();
}

void Browser::runFind(bool backwards, bool newSearch)
{
    auto* view = active();
    if (!view || !m_find || !m_findVisible)
        return;

    const int length = GetWindowTextLengthW(m_find);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(m_find, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));

    view->find(text, backwards, newSearch);
    invalidateChrome();
}

// ---------------------------------------------------------------------------
// Downloads panel
// ---------------------------------------------------------------------------

namespace {

constexpr int kDownloadsWidth = 380;
constexpr int kDownloadsHeader = 48;
constexpr int kDownloadsRow = 68;
constexpr int kDownloadsFooter = 44;
constexpr int kDownloadsMaxHeight = 460;

RECT downloadsActionRect(const RECT& row, int size, int margin)
{
    const int top = row.top + ((row.bottom - row.top) - size) / 2;
    return { row.right - margin - size, top, row.right - margin, top + size };
}

bool isRiskyFile(const std::wstring& path)
{
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos)
        return false;

    std::wstring extension = path.substr(dot);
    for (auto& ch : extension) {
        if (ch >= L'A' && ch <= L'Z')
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }

    static const wchar_t* const risky[] = {
        L".exe", L".msi", L".bat", L".cmd", L".com", L".scr", L".ps1", L".vbs", L".vbe",
        L".js", L".jse", L".wsf", L".lnk", L".jar", L".hta", L".reg", L".dll", L".msix", L".appx"
    };
    for (const wchar_t* item : risky) {
        if (extension == item)
            return true;
    }
    return false;
}

} // namespace

void Browser::createDownloadsPanel()
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass { };
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Browser::downloadsProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = L"LumenDownloads";
        RegisterClassExW(&windowClass);
        registered = true;
    }

    // A small owned pop-up window: it floats above the page views, which a
    // child window of the main window could not reliably do.
    m_downloadsPanel = CreateWindowExW(
        WS_EX_TOOLWINDOW, L"LumenDownloads", L"",
        WS_POPUP,
        0, 0, 10, 10,
        m_window, nullptr, GetModuleHandleW(nullptr), this);
}

LRESULT CALLBACK Browser::downloadsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wParam, lParam);
    }

    auto* browser = reinterpret_cast<Browser*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!browser)
        return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint { };
        HDC dc = BeginPaint(window, &paint);
        RECT client { };
        GetClientRect(window, &client);
        browser->paintBuffered(dc, client, [&](HDC memory) {
            browser->paintDownloadsPanel(memory, static_cast<int>(client.right), static_cast<int>(client.bottom));
        });
        EndPaint(window, &paint);
        return 0;
    }

    case WM_LBUTTONDOWN:
        browser->downloadsClick(POINT { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;

    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track { sizeof(track), TME_LEAVE, window, 0 };
        TrackMouseEvent(&track);
        browser->downloadsMove(POINT { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;
    }

    case WM_MOUSELEAVE:
        browser->downloadsMove(POINT { -1, -1 });
        return 0;

    case WM_MOUSEWHEEL:
        browser->downloadsWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            browser->hideDownloadsPanel(true);
            return 0;
        }
        break;

    case WM_ACTIVATE:
        // Like a menu: clicking anywhere else dismisses it.
        if (LOWORD(wParam) == WA_INACTIVE)
            browser->hideDownloadsPanel(false);
        break;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

int Browser::downloadsPanelHeight() const
{
    const int rows = static_cast<int>(Downloads::instance().items().size());
    const int content = px(kDownloadsHeader) + rows * px(kDownloadsRow) + px(kDownloadsFooter) + px(8);
    return std::min(content, px(kDownloadsMaxHeight));
}

int Browser::downloadsMaxScroll() const
{
    const int rows = static_cast<int>(Downloads::instance().items().size());
    const int listHeight = downloadsPanelHeight() - px(kDownloadsHeader) - px(kDownloadsFooter) - px(8);
    return std::max(0, rows * px(kDownloadsRow) - listHeight);
}

void Browser::layoutDownloadsPanel()
{
    if (!m_downloadsPanel)
        return;

    const ChromeLayout l = layout();
    RECT client { };
    GetClientRect(m_window, &client);

    const int width = px(kDownloadsWidth);
    // The panel grows out of the toolbar rather than popping into existence.
    // Height is the animated one; the content below it is clipped by the
    // window region, so a partially open panel shows the rows from the top.
    const int height = std::max(1, static_cast<int>(std::lround(downloadsPanelHeight() * m_panelAnim.value)));
    const int anchorRight = l.hasDownloads ? static_cast<int>(l.downloadsButton.right) : static_cast<int>(client.right) - px(20);
    const int x = std::clamp(anchorRight - width, px(8), std::max(px(8), static_cast<int>(client.right) - width - px(8)));
    const int y = l.toolbarHeight - px(4);

    POINT origin { x, y };
    ClientToScreen(m_window, &origin);
    SetWindowPos(m_downloadsPanel, HWND_TOP, origin.x, origin.y, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);

    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, px(28), px(28));
    SetWindowRgn(m_downloadsPanel, region, TRUE);

    m_downloadsScroll = std::clamp(m_downloadsScroll, 0, downloadsMaxScroll());
}

void Browser::showDownloadsPanel(bool takeFocus)
{
    if (!m_downloadsPanel || Downloads::instance().items().empty())
        return;

    m_downloadsVisible = true;
    syncAnimations();
    layoutDownloadsPanel();
    InvalidateRect(m_downloadsPanel, nullptr, FALSE);

    // Take focus only when this window is in front, so a download that starts
    // while another program is in use does not steal the keyboard.
    if (takeFocus && GetForegroundWindow() == m_window)
        SetForegroundWindow(m_downloadsPanel);
}

void Browser::hideDownloadsPanel(bool refocusPage)
{
    m_downloadsHiddenAt = GetTickCount64();
    if (!m_downloadsVisible)
        return;

    m_downloadsVisible = false;
    m_downloadsHoverRow = -1;
    m_downloadsHoverAction = false;
    m_downloadsHoverClear = false;
    m_downloadsHoverShowAll = false;

    // The panel collapses back into the toolbar; stepAnimations() is what
    // actually hides the window once the spring reaches zero.
    syncAnimations();

    if (refocusPage && !m_editing && !m_overview) {
        if (auto* view = active())
            view->focus();
    }
}

void Browser::toggleDownloadsPanel()
{
    if (m_downloadsVisible) {
        hideDownloadsPanel(true);
        return;
    }

    // Clicking the button while the panel is open first deactivates (and
    // hides) it; do not open it again straight away.
    if (GetTickCount64() - m_downloadsHiddenAt < 300)
        return;

    showDownloadsPanel(true);
}

void Browser::onDownloadsChanged(bool started)
{
    const bool has = !Downloads::instance().items().empty();
    if (has != m_hadDownloads) {
        m_hadDownloads = has;
        resize(); // the toolbar button appears or disappears
    }

    if (!has)
        hideDownloadsPanel(true);
    else if (started)
        showDownloadsPanel(true);
    else if (m_downloadsVisible) {
        layoutDownloadsPanel();
        InvalidateRect(m_downloadsPanel, nullptr, FALSE);
    }

    // Refresh the full downloads page if it is open, so a download that
    // starts, finishes or is removed updates without the page having to
    // self-reload.
    if (m_downloadsPageTab != static_cast<size_t>(-1) && m_downloadsPageTab < m_tabs.size()) {
        if (auto* view = m_tabs[m_downloadsPageTab].get())
            view->loadHtml(downloadsPageHtml(m_theme.dark));
    }

    // Poll the files on disk while anything is in flight, and stop as soon as
    // nothing is. WebKit's own progress callback has never actually arrived on
    // this port, so the byte count has to come from somewhere real.
    if (Downloads::instance().hasActive())
        SetTimer(m_window, kProgressTimer, 400, nullptr);
    else
        KillTimer(m_window, kProgressTimer);

    invalidateChrome();
}

void Browser::paintDownloadsPanel(HDC dc, int width, int height)
{
    fillSolid(dc, RECT { 0, 0, width, height }, m_theme.dark ? RGB(40, 40, 43) : RGB(250, 250, 252));

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    const float k = scaleFactor();
    const int header = px(kDownloadsHeader);
    const int rowHeight = px(kDownloadsRow);
    const int actionSize = px(26);
    const int actionMargin = px(16);
    const std::vector<DownloadItem>& items = Downloads::instance().items();

    for (size_t i = 0; i < items.size(); ++i) {
        const DownloadItem& item = items[i];
        const int top = header + static_cast<int>(i) * rowHeight - m_downloadsScroll;
        const RECT row { 0, top, width, top + rowHeight };
        if (row.bottom < header || row.top > height)
            continue;

        const bool hot = static_cast<int>(i) == m_downloadsHoverRow;
        if (hot) {
            RECT inset = row;
            InflateRect(&inset, -px(8), -px(2));
            fillRoundedRect(g, inset, px(10), m_theme.pill);
        }

        // File badge: the extension's first letter.
        std::wstring extension;
        const size_t dot = item.filename.find_last_of(L'.');
        if (dot != std::wstring::npos)
            extension = item.filename.substr(dot + 1);
        const int badge = px(36);
        const RECT badgeRect { row.left + px(18), row.top + (rowHeight - badge) / 2, row.left + px(18) + badge, row.top + (rowHeight - badge) / 2 + badge };
        drawSiteBadge(g, badgeRect, badgeLetter(extension), colorFromHue(hueOf(extension)), k * 1.6f);

        const RECT action = downloadsActionRect(row, actionSize, actionMargin);
        const int textLeft = badgeRect.right + px(12);
        const int textRight = action.left - px(10);

        const RECT nameRect { textLeft, row.top + px(10), textRight, row.top + px(10) + px(20) };
        drawLabel(dc, g, item.filename, nameRect, m_theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        std::wstring status;
        switch (item.state) {
        case DownloadItem::State::Active:
            if (item.total > 0)
                status = formatBytes(item.received) + L" of " + formatBytes(item.total);
            else if (item.received > 0)
                status = formatBytes(item.received) + L" \u2014 Downloading";
            else
                status = L"Starting\u2026";
            break;
        case DownloadItem::State::Finished:
            status = formatBytes(item.total > 0 ? item.total : item.received);
            if (!item.source.empty())
                status += L" \u2014 " + item.source;
            break;
        case DownloadItem::State::Failed:
            status = L"Download failed";
            break;
        case DownloadItem::State::Cancelled:
            status = L"Cancelled";
            break;
        }
        const RECT statusRect { textLeft, row.top + px(30), textRight, row.top + px(30) + px(18) };
        drawLabel(dc, g, status, statusRect, m_theme.textSecondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        if (item.state == DownloadItem::State::Active) {
            const int barTop = row.top + px(52);
            const int barHeight = px(4);
            const RECT track { textLeft, barTop, textRight, barTop + barHeight };
            fillRoundedRect(g, track, barHeight / 2, m_theme.pillHover);
            if (item.total > 0) {
                const double fraction = std::clamp(static_cast<double>(item.received) / static_cast<double>(item.total), 0.0, 1.0);
                const RECT fill { textLeft, barTop, textLeft + static_cast<LONG>((textRight - textLeft) * fraction), barTop + barHeight };
                fillRoundedRect(g, fill, barHeight / 2, m_theme.accent);
            } else {
                // Total size unknown (chunked transfer, no Content-Length):
                // draw a centred "pulse" so the row still reads as in-progress.
                const int pulseWidth = std::max(px(48), (textRight - textLeft) / 3);
                const int pulseLeft = textLeft + ((textRight - textLeft) - pulseWidth) / 2;
                const RECT pulse { pulseLeft, barTop, pulseLeft + pulseWidth, barTop + barHeight };
                fillRoundedRect(g, pulse, barHeight / 2, m_theme.accent, 200);
            }
        }

        // Round button: cancel, show in folder, or remove.
        const bool actionHot = hot && m_downloadsHoverAction;
        fillRoundedRect(g, action, actionSize / 2, actionHot ? m_theme.glassHover : m_theme.pillHover);
        if (item.state == DownloadItem::State::Finished)
            drawMagnifierIcon(g, action, m_theme.text, k);
        else
            drawCloseIcon(g, action, m_theme.text, k);
    }

    // Header over the rows, so rows scrolled past the top are hidden.
    fillSolid(dc, RECT { 0, 0, width, header }, m_theme.dark ? RGB(40, 40, 43) : RGB(250, 250, 252));
    const RECT titleRect { px(18), 0, width / 2, header };
    drawLabel(dc, g, L"Downloads", titleRect, m_theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    bool anyFinished = false;
    for (const auto& item : items) {
        if (item.state != DownloadItem::State::Active)
            anyFinished = true;
    }
    const RECT clearRect { width - px(16) - px(64), px(10), width - px(16), header - px(10) };
    if (m_downloadsHoverClear && anyFinished)
        fillRoundedRect(g, clearRect, px(8), m_theme.pill);
    drawLabel(dc, g, L"Clear", clearRect, anyFinished ? m_theme.accent : m_theme.textDisabled,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    fillSolid(dc, RECT { 0, header - 1, width, header }, m_theme.separator);
    strokeRoundedRect(g, RECT { 0, 0, width, height }, px(14), m_theme.glassBorder, std::max(1.0f, k));

    // Footer with the "Show in Page" link, painted last so it covers any
    // row that has scrolled past the bottom edge.
    const int footer = px(kDownloadsFooter);
    const RECT footerRect { 0, height - footer, width, height };
    fillSolid(dc, footerRect, m_theme.dark ? RGB(40, 40, 43) : RGB(250, 250, 252));
    fillSolid(dc, RECT { 0, height - footer, width, height - footer + 1 }, m_theme.separator);

    const RECT showAllRect { px(16), height - footer + px(6), width - px(16), height - px(6) };
    if (m_downloadsHoverShowAll)
        fillRoundedRect(g, showAllRect, px(8), m_theme.pill);
    drawLabel(dc, g, L"Show All in Page", showAllRect, m_theme.accent,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    g.Flush(Gdiplus::FlushIntentionSync);
}

void Browser::downloadsClick(POINT point)
{
    const int width = px(kDownloadsWidth);
    const int header = px(kDownloadsHeader);
    const int footer = px(kDownloadsFooter);
    const int panelHeight = downloadsPanelHeight();
    const int rowHeight = px(kDownloadsRow);
    const std::vector<DownloadItem> items = Downloads::instance().items(); // copy: clicks may change the list

    if (point.y < header) {
        const RECT clearRect { width - px(16) - px(64), px(10), width - px(16), header - px(10) };
        if (PtInRect(&clearRect, point))
            Downloads::instance().clearFinished();
        return;
    }

    if (point.y >= panelHeight - footer) {
        const RECT showAllRect { px(16), panelHeight - footer + px(6), width - px(16), panelHeight - px(6) };
        if (PtInRect(&showAllRect, point)) {
            hideDownloadsPanel(false);
            showDownloadsPage();
        }
        return;
    }

    for (size_t i = 0; i < items.size(); ++i) {
        const int top = header + static_cast<int>(i) * rowHeight - m_downloadsScroll;
        const RECT row { 0, top, width, top + rowHeight };
        if (!PtInRect(&row, point))
            continue;

        const DownloadItem& item = items[i];
        const RECT action = downloadsActionRect(row, px(26), px(16));
        if (PtInRect(&action, point)) {
            if (item.state == DownloadItem::State::Active)
                Downloads::instance().cancel(item.id);
            else if (item.state == DownloadItem::State::Finished)
                revealDownloadedFile(item.path);
            else
                Downloads::instance().remove(item.id);
            return;
        }

        if (item.state == DownloadItem::State::Finished) {
            hideDownloadsPanel(false);
            openDownloadedFile(item.path);
        }
        return;
    }
}

void Browser::downloadsMove(POINT point)
{
    const int width = px(kDownloadsWidth);
    const int header = px(kDownloadsHeader);
    const int footer = px(kDownloadsFooter);
    const int panelHeight = downloadsPanelHeight();
    const int rowHeight = px(kDownloadsRow);
    const std::vector<DownloadItem>& items = Downloads::instance().items();

    int hoverRow = -1;
    bool hoverAction = false;
    bool hoverClear = false;
    bool hoverShowAll = false;

    if (point.x >= 0 && point.y >= 0) {
        if (point.y < header) {
            const RECT clearRect { width - px(16) - px(64), px(10), width - px(16), header - px(10) };
            hoverClear = PtInRect(&clearRect, point) != FALSE;
        } else if (point.y >= panelHeight - footer) {
            const RECT showAllRect { px(16), panelHeight - footer + px(6), width - px(16), panelHeight - px(6) };
            hoverShowAll = PtInRect(&showAllRect, point) != FALSE;
        } else {
            for (size_t i = 0; i < items.size(); ++i) {
                const int top = header + static_cast<int>(i) * rowHeight - m_downloadsScroll;
                const RECT row { 0, top, width, top + rowHeight };
                if (!PtInRect(&row, point))
                    continue;

                hoverRow = static_cast<int>(i);
                const RECT action = downloadsActionRect(row, px(26), px(16));
                hoverAction = PtInRect(&action, point) != FALSE;
                break;
            }
        }
    }

    if (hoverRow == m_downloadsHoverRow && hoverAction == m_downloadsHoverAction
        && hoverClear == m_downloadsHoverClear && hoverShowAll == m_downloadsHoverShowAll)
        return;

    m_downloadsHoverRow = hoverRow;
    m_downloadsHoverAction = hoverAction;
    m_downloadsHoverClear = hoverClear;
    m_downloadsHoverShowAll = hoverShowAll;
    InvalidateRect(m_downloadsPanel, nullptr, FALSE);
}

void Browser::downloadsWheel(int delta)
{
    const int step = MulDiv(delta, px(60), 120);
    m_downloadsScroll = std::clamp(m_downloadsScroll - step, 0, downloadsMaxScroll());
    InvalidateRect(m_downloadsPanel, nullptr, FALSE);
}

void Browser::openDownloadedFile(const std::wstring& path)
{
    if (isRiskyFile(path)) {
        const int answer = MessageBoxW(
            m_window,
            L"This file can run programs on your computer. Only open files from sources you trust.\n\nOpen it anyway?",
            L"Open Download",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (answer != IDYES)
            return;
    }

    ShellExecuteW(m_window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Browser::revealDownloadedFile(const std::wstring& path)
{
    const std::wstring arguments = L"/select,\"" + path + L"\"";
    ShellExecuteW(m_window, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
}

void Browser::showDownloadsPage()
{
    // Open a new tab that lists every download. The browser pushes an
    // updated HTML snapshot to it whenever Downloads::onChanged fires, so
    // the page itself does not have to self-reload.
    auto view = m_host.createView(m_window);
    if (!view)
        return;

    WebView* raw = view.get();
    attachCallbacks(raw);
    m_tabs.push_back(std::move(view));
    m_downloadsPageTab = m_tabs.size() - 1;

    activateTab(m_downloadsPageTab);
    raw->loadHtml(downloadsPageHtml(m_theme.dark));
    invalidateChrome();
}

void Browser::openSettings()
{
    // If a Settings tab is already open, just bring it forward.
    if (m_settingsPageTab != static_cast<size_t>(-1) && m_settingsPageTab < m_tabs.size()) {
        activateTab(m_settingsPageTab);
        return;
    }

    auto view = m_host.createView(m_window);
    if (!view)
        return;

    WebView* raw = view.get();
    attachCallbacks(raw);
    m_tabs.push_back(std::move(view));
    m_settingsPageTab = m_tabs.size() - 1;

    // Always open on the first pane, the way Safari does.
    Settings::instance().setView(L"general");

    activateTab(m_settingsPageTab);
    raw->loadHtml(settingsPageHtml(m_theme.dark));
    invalidateChrome();
}

void Browser::onSettingsChanged()
{
    // The Settings UI fires lumen:settings?key=X&value=Y (or ?view=x, or an
    // __action__ one-shot) for every change; the URL handler in
    // webkit_host.cpp applies it and posts this message. Re-render the open
    // Settings tab so the new value shows up.
    if (m_settingsPageTab == static_cast<size_t>(-1) || m_settingsPageTab >= m_tabs.size())
        return;
    if (auto* view = m_tabs[m_settingsPageTab].get())
        view->loadHtml(settingsPageHtml(m_theme.dark));
}

void Browser::onClearHistory()
{
    // "Clear History and Website Data" in Privacy & Security. The Library is
    // ours, so wipe it here and let the sidebar repaint with the new list.
    m_library.clearHistory();
    m_sidebarScroll = 0;
    m_sidebarHoverRow = -1;
    invalidateChrome();
}


} // namespace lumen
