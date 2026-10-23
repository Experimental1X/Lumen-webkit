#pragma once

#include <windows.h>

#include <WebKit/WKContext.h>
#include <WebKit/WKContextConfigurationRef.h>
#include <WebKit/WKPage.h>
#include <WebKit/WKPageConfigurationRef.h>
#include <WebKit/WKPageStateClient.h>
#include <WebKit/WKPreferencesRef.h>
#include "webkit_ref.h"
#include <WebKit/WKString.h>
#include <WebKit/WKURL.h>
#include <WebKit/WKView.h>
#include <WebKit/WKWebsiteDataStoreConfigurationRef.h>
#include <WebKit/WKWebsiteDataStoreRef.h>

#include <functional>
#include <memory>
#include <string>

namespace lumen {

class WebKitHost;
class WebView;

// What WebKit's callbacks point at. It outlives the WebView (a few bytes per
// tab) so that a late callback after the view is gone finds `owner == nullptr`
// instead of a dangling pointer.
struct CallbackTarget {
    WebView* owner { nullptr };
};

// One web view (one tab's worth of content). The browser UI owns these.
class WebView {
public:
    ~WebView();

    // Last captured picture of the page, shown in the tab overview.
    HBITMAP thumbnail() const { return m_thumbnail; }
    void setThumbnail(HBITMAP bitmap);

    // Tells WebKit this page is finished. Must happen before the view is
    // released, otherwise WebKit keeps calling into a destroyed view.
    void closePage();

    HWND hwnd() const;
    WKPageRef page() const;

    void load(const std::wstring& url);
    void loadHtml(const std::string& utf8Html);
    void setVisible(bool visible);
    void focus();
    // What setVisible() last applied. WebKit re-does the whole attach when told
    // to, so it must only be told on an actual change.
    bool m_visible { false };
    void setBackingScale(double scale);
    double zoom() const;
    void setZoom(double factor);
    void goBack();
    void goForward();
    void reloadOrStop();

    bool canGoBack() const;
    bool canGoForward() const;
    double progress() const;

    const std::wstring& title() const { return m_title; }
    const std::wstring& url() const { return m_url; }

    // Called on the UI thread whenever title, URL, progress or navigation
    // state changes.
    std::function<void()> onChanged;

    // The page wants a new tab (a link with target=_blank, or window.open).
    // Must create the tab and return its page, or null to refuse.
    std::function<WKPageRef(WKPageConfigurationRef)> onNewPage;

    // The page asked to be closed (window.close()).
    std::function<void()> onCloseRequested;

    // Find in page. `countMatches` also counts all matches for the status text.
    void find(const std::wstring& text, bool backwards, bool countMatches);
    void hideFind();
    // -1: nothing searched yet; 0: not found; otherwise the number of matches.
    int findMatches() const { return m_findMatches; }

private:
    friend class WebKitHost;

    void refreshState();
    static WebView* ownerOf(const void* clientInfo);
    static HWND windowOf(WebView* view);
    static void didChange(const void* clientInfo);
    static void didChangeTitleOrUrl(const void* clientInfo);

    // Navigation policy: decides what becomes a download.
    static void decideNavigationAction(WKPageRef page, WKNavigationActionRef action, WKFramePolicyListenerRef listener, WKTypeRef userData, const void* clientInfo);
    static void decideNavigationResponse(WKPageRef page, WKNavigationResponseRef response, WKFramePolicyListenerRef listener, WKTypeRef userData, const void* clientInfo);
    static void actionBecameDownload(WKPageRef page, WKNavigationActionRef action, WKDownloadRef download, const void* clientInfo);
    static void responseBecameDownload(WKPageRef page, WKNavigationResponseRef response, WKDownloadRef download, const void* clientInfo);
    static void contextMenuCreatedDownload(WKPageRef page, WKDownloadRef download, const void* clientInfo);

    // UI client: new windows, dialogs and the file chooser.
    static WKPageRef createNewPage(WKPageRef page, WKPageConfigurationRef configuration, WKNavigationActionRef action, WKWindowFeaturesRef features, const void* clientInfo);
    static void closeRequested(WKPageRef page, const void* clientInfo);
    static void runAlert(WKPageRef page, WKStringRef text, WKFrameRef frame, WKSecurityOriginRef origin, WKPageRunJavaScriptAlertResultListenerRef listener, const void* clientInfo);
    static void runConfirm(WKPageRef page, WKStringRef text, WKFrameRef frame, WKSecurityOriginRef origin, WKPageRunJavaScriptConfirmResultListenerRef listener, const void* clientInfo);
    static void runPrompt(WKPageRef page, WKStringRef text, WKStringRef defaultValue, WKFrameRef frame, WKSecurityOriginRef origin, WKPageRunJavaScriptPromptResultListenerRef listener, const void* clientInfo);
    static void runOpenPanel(WKPageRef page, WKFrameRef frame, WKOpenPanelParametersRef parameters, WKOpenPanelResultListenerRef listener, const void* clientInfo);

    // Find client.
    static void didFindString(WKPageRef page, WKStringRef string, unsigned matchCount, const void* clientInfo);
    static void didFailToFindString(WKPageRef page, WKStringRef string, const void* clientInfo);
    static void didCountStringMatches(WKPageRef page, WKStringRef string, unsigned matchCount, const void* clientInfo);

    Ref<WKViewRef> m_view;
    std::wstring m_title;
    std::wstring m_url;
    HBITMAP m_thumbnail { nullptr };
    CallbackTarget* m_target { nullptr };
    bool m_closed { false };
    bool m_leakOnDestroy { false };
    int m_findMatches { -1 };
};

// Owns the WebKit context shared by every view in the process.
class WebKitHost {
public:
    bool initialize(std::wstring& error);

    // `configuration` is null for a normal tab; for a page the web content
    // asked for (window.open) it is the configuration WebKit supplied.
    std::unique_ptr<WebView> createView(HWND parent, WKPageConfigurationRef configuration = nullptr);

private:
    Ref<WKWebsiteDataStoreConfigurationRef> m_dataStoreConfiguration;
    Ref<WKWebsiteDataStoreRef> m_dataStore;
    Ref<WKContextConfigurationRef> m_contextConfiguration;
    Ref<WKContextRef> m_context;
    Ref<WKPreferencesRef> m_preferences;
    Ref<WKPageConfigurationRef> m_pageConfiguration;
};

} // namespace lumen
