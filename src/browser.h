#pragma once

#include "gfx.h"
#include "library.h"
#include "settings.h"
#include "webkit_host.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
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
    kCmdFind,
    kCmdFindNext,
    kCmdFindPrevious,
    kCmdDownloads,
    kCmdMinimize,
    kCmdMaximize,
    kCmdCloseWindow,
    kCmdAddressControl = 1500,
    kCmdFindControl = 1501,
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
    TabClose,
    Downloads,
    FindPrevious,
    FindNext,
    FindDone
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
    int findBarHeight { 0 };
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

    bool hasDownloads { false };
    RECT downloadsButton { };

    RECT rightGroup { };
    RECT shareButton { };
    RECT newTabButton { };
    RECT overviewButton { };

    RECT tabBar { };
    std::vector<RECT> tabs;

    RECT findBar { };
    RECT findField { };
    RECT findPrevious { };
    RECT findNext { };
    RECT findStatus { };
    RECT findDone { };
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
    // `requested` is the paint structure's rectangle - the area the system
    // says is stale. It is passed in because BeginPaint has already emptied
    // the window's update region by the time this runs.
    void onPaint(HDC target, const RECT& requested);
    void onMouseMove(POINT point);
    void onMouseLeave();
    void onNcMouseMove();
    bool onLeftButtonDown(POINT point);
    void onLeftButtonUp();
    LRESULT onNcHitTest(LRESULT defaultHit, POINT screenPoint);
    void onDpiChanged();
    void onSettingChange(LPARAM lParam);
    void onSetFocus();
    bool onMouseWheel(WPARAM wParam, LPARAM lParam);
    LRESULT onCtlColorEdit(HDC dc, HWND control);
    void onCommand(UINT command);
    bool onKeyDown(WPARAM key);
    void onTimer(UINT_PTR id);
    void onControlNotify(HWND control, UINT code);

    // Pumped by the WM_LUMEN_DOWNLOAD_CHANGED / WM_LUMEN_SETTINGS_CHANGED
    // message handlers in main.cpp. These callbacks can fire on WebKit's IPC
    // threads, so the resulting UI work has to land on this thread.
    void onDownloadsChanged(bool started);
    void onSettingsChanged();
    void onClearHistory();

    // Opens the Settings page in a new tab. Brings an existing Settings
    // tab forward instead of duplicating it.
    void openSettings();

    int chromeHeight() const;
    bool trackingMouse() const { return m_trackingMouse; }
    void setTrackingMouse(bool value) { m_trackingMouse = value; }

    // The animation clock. The message pump in main.cpp owns the frame rate:
    // it asks how long to sleep, sleeps, drains the queue, and then calls
    // idleTick() once the queue has run dry. Nothing in the program runs on a
    // WM_TIMER any more - see the note on highResolutionNow() in browser.cpp
    // for why a timer could not carry a 60 Hz animation.
    DWORD millisecondsUntilNextFrame() const;
    void idleTick();

private:
    // --- motion ---
    // Every transition in the chrome is a damped spring integrated against the
    // clock, not a fixed-duration tween. Apple animates layout with
    // spring(response:dampingFraction:) and its defaults are 0.55 / 0.825 for
    // layout changes and 0.15 / 0.86 for control feedback, so those are the
    // numbers used here: a layout spring that settles in about half a second
    // with a barely perceptible overshoot, and a much snappier one for a
    // button that has been pressed.
    struct Spring {
        double value { 0.0 };
        double target { 0.0 };
        double velocity { 0.0 };

        void snap(double to)
        {
            value = to;
            target = to;
            velocity = 0.0;
        }

        void animateTo(double to) { target = to; }

        bool settled() const { return value == target && velocity == 0.0; }

        bool step(double dt, double response, double damping);
    };

    static constexpr double kLayoutResponse = 0.55;
    static constexpr double kLayoutDamping = 0.825;
    static constexpr double kControlResponse = 0.15;
    static constexpr double kControlDamping = 0.86;

    // Advances every spring and repaints if any of them moved. Returns true
    // while at least one is still travelling.
    bool stepAnimations();
    void syncAnimations();
    // True while any spring is still in flight. The pump uses it to decide
    // whether to wake up often or back off, so it has to be cheap and it has
    // to be true the instant syncAnimations() aims something.
    bool animating() const;
    // Current width of the sidebar, in pixels, mid-transition included.
    int sidebarWidthNow() const;
    // Width the sidebar is heading for. resize() places the web view with this
    // rather than with sidebarWidthNow(), so a toggle moves the page once
    // instead of once per animation tick - see resize().
    int sidebarSettledWidth() const;

    Spring m_sidebarAnim;
    Spring m_panelAnim;
    Spring m_overviewAnim;
    Spring m_findAnim;
    Spring m_addressAnim;
    // Wall clock for the springs, in seconds on the QueryPerformanceCounter
    // epoch. Negative means "no tick yet", which is what tells the first tick
    // to invent a plausible dt instead of measuring one.
    double m_lastAnimTime { -1.0 };
    // When the next animation frame is due, on the same clock.
    double m_nextFrameTime { 0.0 };
    // Set when a tick has asked for a repaint and cleared when the repaint has
    // actually happened. It stops the springs being advanced again while the
    // frame they asked for is still waiting to be drawn - which would move the
    // WebKit child several times for one visible frame, and moving it is what
    // makes it come back black.
    bool m_framePending { false };
    // When the pending frame was asked for, on the same clock. If the paint it
    // asked for never arrives - a minimised window, a covered window - the
    // animation must not be allowed to wait for it forever, so this is the
    // escape hatch.
    double m_framePendingSince { 0.0 };
    // Index of the toolbar button currently held down, and how far into the
    // press it is. -1 when nothing is pressed.
    int m_pressTarget { -1 };
    Spring m_pressAnim;

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
    void attachCallbacks(WebView* view);
    WKPageRef openPageFromWebKit(WKPageConfigurationRef configuration);
    void closeTabForView(WebView* view);
    void updateWindowTitle();
    // wholeClient = false limits the repaint to the chrome strip and the
    // sidebar column, which is what an animation tick wants; a real window
    // resize passes true because the page area really did change shape.
    void resize(bool wholeClient = true);
    void invalidateSidebarColumn();
    void invalidateContentArea();
    RECT contentArea() const;
    void paintBuffered(HDC target, const RECT& area, const POINT& origin, const std::function<void(HDC)>& draw,
        const RECT* extent = nullptr);
    // Reused back buffer for paintBuffered. A window-sized bitmap is about
    // eight megabytes; making a fresh one per WM_PAINT was costing more than
    // the drawing did, and the flicker came from blitting an untouched area
    // of it back to the screen.
    HDC m_backDC { nullptr };
    HBITMAP m_backBitmap { nullptr };
    HGDIOBJ m_backPrevious { nullptr };
    LONG m_backWidth { 0 };
    LONG m_backHeight { 0 };
    void releaseBackBuffer();
    // Scratch surface the overview is drawn into before it goes on screen.
    //
    // A plain 32bpp DIB section rather than a Gdiplus::Bitmap: GDI+ has to be
    // given an HDC to draw into, and the only way to get one out of a Bitmap is
    // GetHDC/ReleaseHDC around every frame, which converts the bitmap on the
    // way in and back on the way out. Handing Gdiplus::Graphics a memory DC it
    // already owns is the fast path, and at 1920x700 sixty times a second the
    // difference is the whole frame budget.
    void releaseOverviewSurface();
    bool ensureOverviewSurface(int width, int height);
    HDC m_overviewDC { nullptr };
    HBITMAP m_overviewBitmap { nullptr };
    // HGDIOBJ, not HBITMAP. SelectObject returns HGDIOBJ, which is void*, and
    // void* does not convert implicitly to HBITMAP - that is error C2440, and
    // it is what the first CI build of this file failed on. m_backPrevious has
    // always been HGDIOBJ for the same reason.
    HGDIOBJ m_overviewPrevious { nullptr };
    void* m_overviewBits { nullptr };
    LONG m_overviewWidth { 0 };
    LONG m_overviewHeight { 0 };
    void drawLabel(HDC dc, Gdiplus::Graphics& g, const std::wstring& text, RECT rect, COLORREF color, UINT flags) const;
    int measureLabel(HDC dc, Gdiplus::Graphics& g, const std::wstring& text) const;
    // measureLabelWidthGdiPlus is a full GDI+ text layout, and the overview asks
    // for one per card per frame - eleven of them, sixty times a second, to
    // answer a question that only changes when the font or the title does.
    // updateFont() empties this.
    Gdiplus::REAL cachedLabelWidth(Gdiplus::Graphics& g, const std::wstring& text);
    std::unordered_map<std::wstring, Gdiplus::REAL> m_labelWidths;

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

    // --- address bar suggestions ---
    // One row of the dropdown that appears under the address field while the
    // user types, the way Safari offers matching history and bookmarks.
    struct Suggestion {
        std::wstring title;   // page title, or the query for a search row
        std::wstring url;     // always a resolved URL, even for a search row
        std::wstring host;    // host to badge the row with; empty on a search
        bool isSearch { false };
    };

    void createSuggestionsPanel();
    void rebuildSuggestions(const std::wstring& text);
    void updateSuggestions();
    void showSuggestions();
    void hideSuggestions();
    int suggestionsHeight() const;
    int suggestionsMaxScroll() const;
    void layoutSuggestionsPanel();
    RECT suggestionRow(int index) const;
    RECT suggestionNewTab(int index) const;
    void paintSuggestions(HDC dc, int width, int height);
    void suggestionsClick(POINT point);
    void suggestionsMove(POINT point);
    void suggestionsWheel(int delta);
    void suggestionsKey(int key);
    void openSuggestion(int index, bool inNewTab);
    static LRESULT CALLBACK suggestionsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    // --- find in page ---
    void createFindControl();
    void placeFindEdit(const ChromeLayout& l) const;
    void showFind();
    void hideFind();
    void runFind(bool backwards, bool newSearch);
    static LRESULT CALLBACK findProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    // --- downloads panel ---
    void createDownloadsPanel();
    int downloadsPanelHeight() const;
    int downloadsMaxScroll() const;
    void layoutDownloadsPanel();
    void showDownloadsPanel(bool takeFocus);
    void hideDownloadsPanel(bool refocusPage);
    void toggleDownloadsPanel();
    void paintDownloadsPanel(HDC dc, int width, int height);
    void downloadsClick(POINT point);
    void downloadsMove(POINT point);
    void downloadsWheel(int delta);
    void openDownloadedFile(const std::wstring& path);
    void revealDownloadedFile(const std::wstring& path);
    void showDownloadsPage();
    static LRESULT CALLBACK downloadsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

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
    // Where the tab overview's grid lives, and how wide it is.
    //
    // The grid is in the content area BESIDE the sidebar, not over it. Safari
    // shows both at once: the sidebar is a column of its own and the grid
    // fills the rest of the content area. The overview used to be painted
    // across the whole content area, which hid the sidebar completely - the
    // "the sidebar closes itself when I open the tab overview" report.
    int overviewGridLeft() const;
    int overviewGridWidth(int clientWidth) const;
    void showOverview();
    void hideOverview();
    void setOverview(bool up);
    // Puts the page view back on screen once the overview has finished
    // easing away. A one-shot, guarded by m_pageRestorePending.
    void restorePageIfPending();
    void captureThumbnail(WebView& view);
    bool overviewClick(POINT local, int width, int height);
    void overviewMove(POINT local, int width);
    // The tab overview. Drawn into its own scratch surface and handed to the
    // window in one copy, straight to `destination` - the content area's
    // top-left in client coordinates.
    void paintOverview(HDC dc, int width, int height, const POINT& destination);
    COLORREF overviewBackdrop() const;
    // The tab grid itself, drawn straight into the scratch surface so the cards
    // can be drawn staggered and transformed without GDI+ having to repaint the
    // live surface mid-transform.
    //
    // `t` is the same 0..1 spring value paintOverview has: it drives each card's
    // own progress on a stagger, so `t` is the grid as a whole and every card
    // reads a delayed slice of it.
    void paintOverviewContent(Gdiplus::Graphics& g, int width, int height, double t);
    // Safari's tab-select transition: the card that was clicked grows from its
    // slot in the grid into the whole content area while the grid dims behind
    // it. Without it the overview simply disappears and the page is abruptly
    // there, which is what "there is no tab select animation" was describing.
    void paintSelectingCard(Gdiplus::Graphics& g, int gridLeft, int gridWidth,
        int height, double progress);

    // --- sidebar (Bookmarks, Reading List, History) ---
    struct SidebarLayout {
        int width { 0 };
        int height { 0 };
        int rowHeight { 0 };
        RECT segmented { };
        RECT segments[3] { };
        RECT list { };
        RECT clearButton { };
        // Always-visible gear button in the bottom-left corner that opens
        // the Settings tab. Computed in sidebarLayout; clicked in sidebarClick;
        // drawn in paintSidebar.
        RECT gearButton { };
    };
    SidebarLayout sidebarLayout(int width, int height) const;
    const std::vector<LibraryEntry>& sidebarEntries() const;
    RECT sidebarRow(const SidebarLayout& layout, size_t index) const;
    RECT sidebarRowClose(const RECT& row) const;
    int sidebarMaxScroll(int height) const;
    // False when the active tab is one of the built-in pages (Settings,
    // Downloads). Those own the whole content area, so the sidebar steps
    // aside for them even when it is switched on.
    bool sidebarVisibleNow() const;
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
    HBRUSH m_findBrush { nullptr };

    HWND m_find { nullptr };
    WNDPROC m_findOriginal { nullptr };
    bool m_findVisible { false };

    HWND m_downloadsPanel { nullptr };
    bool m_downloadsVisible { false };
    bool m_hadDownloads { false };

    // --- address bar suggestions ---
    HWND m_suggestionsPanel { nullptr };
    std::vector<Suggestion> m_suggestions;
    int m_suggestionsSelected { -1 }; // driven by the arrow keys
    int m_suggestionsHover { -1 };    // driven by the mouse
    int m_suggestionsScroll { 0 };
    int m_suggestionsWidth { 0 };     // cached panel width; hit tests need it
    bool m_suggestionsHoverNewTab { false };
    bool m_suggestionsVisible { false };
    // Set once the first EN_CHANGE after focusing has been seen. Focusing the
    // field seeds it with the current URL, which fires EN_CHANGE by itself;
    // without this the dropdown would cover the page the moment you click the
    // field, before the user has typed anything.
    bool m_suggestionsPrimed { false };

    int m_downloadsScroll { 0 };
    int m_downloadsHoverRow { -1 };
    // The row's right-hand button: Stop while live, Show in folder once
    // finished, Remove once failed or stopped.
    bool m_downloadsHoverAction { false };
    // The Pause / Resume button immediately to its left. Separate from the
    // primary so the two highlight independently - they are different actions
    // and used to be collapsed into one ambiguous toggle.
    bool m_downloadsHoverToggle { false };
    bool m_downloadsHoverClear { false };
    bool m_downloadsHoverShowAll { false };
    ULONGLONG m_downloadsHiddenAt { 0 };
    gfx::Theme m_theme;

    // Tab that hosts the full downloads page (lumen:Downloads), or -1 if no
    // such tab is open. The browser pushes an updated HTML snapshot to the
    // view whenever Downloads::onChanged fires, so the page itself does not
    // have to self-reload.
    size_t m_downloadsPageTab { static_cast<size_t>(-1) };
    // When that snapshot was last pushed. Progress is measured off the disk
    // every 400 ms, and a full WKPageLoadHTML every one of those lands a WebKit
    // navigation twice a second on the thread that also has to answer clicks,
    // so the push is rate limited and the page's own reload picks up whatever
    // fell in between.
    ULONGLONG m_downloadsPagePushedAt { 0 };

    // Same idea for the Settings tab. The Settings HTML uses form submits
    // and toggles; the browser refreshes the page after every
    // lumen:settings URL handler so the new value renders.
    size_t m_settingsPageTab { static_cast<size_t>(-1) };

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
    // Polls the destination files of in-flight downloads. didWriteData does
    // fire on this build; see downloads.h for the data path that's still
    // stuck on it. Polling covers every active transfer regardless of which
    // path wrote the file.
    static constexpr UINT_PTR kProgressTimer = 2;
    std::vector<ClosedTab> m_closedTabs;
    void reapTabs();
    size_t m_active { 0 };

    bool m_overview { false };
    int m_overviewScroll { 0 };
    int m_overviewHoverCard { -1 };
    bool m_overviewHoverClose { false };
    // Set by showOverview(), which hides every tab view, and cleared by the
    // one-shot in stepAnimations() that puts the active one back. Without it
    // that restore would re-run on every tick of every later animation.
    bool m_pageRestorePending { false };
    // Which card the tab-select transition is expanding, and the rect it is
    // growing out of. -1 when no selection is in flight. Cleared on the tick
    // that lands the transition - it must not survive into the next overview
    // visit, or the opening would play the previous selection backwards.
    int m_selectIndex { -1 };
    RECT m_selectFrom { };

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
