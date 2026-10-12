#pragma once

#include "gfx.h"
#include "library.h"
#include "webkit_host.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lumen {

// Command identifiers shared by menus, accelerators and toolbar buttons.
enum Command : UINT {
    kCmdNewTab = 1001,
    kCmdCloseTab,
    kCmdFocusAddress,
    kCmdReload,
    kCmdBack,
    kCmdForward,
    kCmdZoomIn,
    kCmdZoomOut,
    kCmdZoomReset,
    kCmdNextTab,
    kCmdPrevTab,
    kCmdStartPage,
    kCmdCopyLink,
    kCmdSidebar,
    kCmdTabOverview,
    kCmdBookmarkPage,
    kCmdReadingList,
    kCmdClearHistory,
    kCmdMinimize,
    kCmdMaximize,
    kCmdCloseWindow,
    kCmdAddressControl = 1500,
    kCmdTabBase = 3000,
};

enum class Target {
    None,
    CloseWindow,
    MinimizeWindow,
    MaximizeWindow,
    Sidebar,
    Back,
    Forward,
    PageMenu,
    Address,
    Reload,
    Share,
    NewTab,
    TabOverview,
    Tab,
    TabClose
};

struct HitResult {
    Target target { Target::None };
    size_t tab { 0 };

    bool operator==(const HitResult& other) const
    {
        return target == other.target && tab == other.tab;
    }
    bool operator!=(const HitResult& other) const { return !(*this == other); }
};

// Positions of everything in the toolbar row and the tab bar row.
struct ChromeLayout {
    int toolbarHeight { 0 };
    int tabBarHeight { 0 };
    int totalHeight { 0 };

    RECT closeLight { };
    RECT minimizeLight { };
    RECT maximizeLight { };

    RECT sidebar { };
    RECT navGroup { };
    RECT back { };
    RECT forward { };

    RECT pill { };
    RECT pageMenu { };
    RECT reload { };

    RECT rightGroup { };
    RECT shareButton { };
    RECT newTabButton { };
    RECT overviewButton { };

    RECT tabBar { };
    std::vector<RECT> tabs;
};

// The browser: owns the tabs, draws the Safari-style chrome, and handles
// input for the main window. main.cpp forwards window messages to it.
class Browser {
public:
    ~Browser();

    bool create(HWND window, std::wstring& error);
    void destroy();

    // Window message entry points.
    void onSize();
    void onPaint(HDC target);
    void onMouseMove(POINT point);
    void onMouseLeave();
    void onNcMouseMove();
    bool onLeftButtonDown(POINT point);
    LRESULT onNcHitTest(LRESULT defaultHit, POINT screenPoint);
    void onDpiChanged();
    void onSettingChange(LPARAM lParam);
    void onSetFocus();
    bool onMouseWheel(WPARAM wParam, LPARAM lParam);
    LRESULT onCtlColorEdit(HDC dc, HWND control);
    void onCommand(UINT command);
    bool onKeyDown(WPARAM key);
    void onTimer(UINT_PTR id);

    int chromeHeight() const;
    bool trackingMouse() const { return m_trackingMouse; }
    void setTrackingMouse(bool value) { m_trackingMouse = value; }

private:
    // --- metrics ---
    UINT dpi() const;
    int px(int logical) const;
    float scaleFactor() const;
    void invalidateChrome() const;
    ChromeLayout layout() const;
    RECT tabCloseRect(const RECT& tab) const;

    // --- theme and font ---
    void applyTheme(bool dark);
    void updateFont();

    // --- tabs ---
    WebView* active() const;
    bool newTab(const std::wstring& url);
    void closeTab(size_t index);
    void activateTab(size_t index);
    void loadStartPage(WebView& view);
    void onViewChanged(WebView* view);
    void updateWindowTitle();
    void resize();
    RECT contentArea() const;
    void paintBuffered(HDC target, const RECT& area, const std::function<void(HDC)>& draw);
    void drawLabel(HDC dc, Gdiplus::Graphics& g, const std::wstring& text, RECT rect, COLORREF color, UINT flags) const;
    int measureLabel(HDC dc, Gdiplus::Graphics& g, const std::wstring& text) const;

    // --- address field ---
    void createAddressControl();
    void placeAddressBar(const ChromeLayout& l) const;
    void beginAddressEdit();
    void endAddressEdit();
    void commitAddress();
    void cancelAddressEdit();
    std::wstring addressDisplayText(bool& secure) const;
    static std::wstring normalizeInput(const std::wstring& input);
    void navigate(const std::wstring& input);
    static LRESULT CALLBACK addressProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    // --- input and popups ---
    HitResult hitTest(POINT point, const ChromeLayout& l) const;
    bool hitIsInteractive(POINT point) const;
    void updateHover(POINT point);
    void clearHover();
    void showPageMenu();
    void showShareMenu();
    void copyLink();
    void zoomBy(double factor);
    void bookmarkCurrent();
    void readingListCurrent();

    // --- tab overview (the grid of page previews) ---
    struct OverviewCard {
        RECT card { };
        RECT thumb { };
        RECT close { };
        RECT title { };
    };
    std::vector<OverviewCard> overviewLayout(int width) const;
    int overviewMaxScroll(int width, int height) const;
    void showOverview();
    void hideOverview();
    void captureThumbnail(WebView& view);
    bool overviewClick(POINT local, int width, int height);
    void overviewMove(POINT local, int width);
    void paintOverview(HDC dc, int width, int height);

    // --- sidebar (Bookmarks, Reading List, History) ---
    struct SidebarLayout {
        int width { 0 };
        int height { 0 };
        int rowHeight { 0 };
        RECT segmented { };
        RECT segments[3] { };
        RECT list { };
        RECT clearButton { };
    };
    SidebarLayout sidebarLayout(int height) const;
    const std::vector<LibraryEntry>& sidebarEntries() const;
    RECT sidebarRow(const SidebarLayout& layout, size_t index) const;
    RECT sidebarRowClose(const RECT& row) const;
    int sidebarMaxScroll(int height) const;
    void toggleSidebar();
    bool sidebarClick(POINT local, int height);
    void sidebarMove(POINT local, int height);
    void paintSidebar(HDC dc, int width, int height);

    // --- painting ---
    void paintChrome(HDC dc, const ChromeLayout& l);

    HWND m_window { nullptr };
    HWND m_address { nullptr };
    WNDPROC m_addressOriginal { nullptr };
    HFONT m_font { nullptr };
    HBRUSH m_addressBrush { nullptr };
    gfx::Theme m_theme;

    WebKitHost m_host;
    Library m_library;
    std::vector<std::unique_ptr<WebView>> m_tabs;
    // Closed tabs wait here for a few seconds before being released, so that
    // WebKit can finish shutting their pages down.
    struct ClosedTab {
        std::unique_ptr<WebView> view;
        ULONGLONG closedAt { 0 };
    };
    static constexpr UINT_PTR kReapTimer = 1;
    std::vector<ClosedTab> m_closedTabs;
    void reapTabs();
    size_t m_active { 0 };

    bool m_overview { false };
    int m_overviewScroll { 0 };
    int m_overviewHoverCard { -1 };
    bool m_overviewHoverClose { false };

    bool m_sidebarVisible { false };
    int m_sidebarMode { 0 };
    int m_sidebarScroll { 0 };
    int m_sidebarHoverRow { -1 };
    bool m_sidebarHoverClose { false };

    HitResult m_hover;
    bool m_trackingMouse { false };
    bool m_editing { false };
    bool m_forwardingWheel { false };
};

} // namespace lumen
