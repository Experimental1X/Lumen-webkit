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

    HWND hwnd() const;
    WKPageRef page() const;

    void load(const std::wstring& url);
    void loadHtml(const std::string& utf8Html);
    void setVisible(bool visible);
    void focus();
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

private:
    friend class WebKitHost;

    void refreshState();
    static void didChange(const void* clientInfo);
    static void didChangeTitleOrUrl(const void* clientInfo);

    Ref<WKViewRef> m_view;
    std::wstring m_title;
    std::wstring m_url;
    HBITMAP m_thumbnail { nullptr };
    CallbackTarget* m_target { nullptr };
};

// Owns the WebKit context shared by every view in the process.
class WebKitHost {
public:
    bool initialize(std::wstring& error);
    std::unique_ptr<WebView> createView(HWND parent);

private:
    Ref<WKWebsiteDataStoreConfigurationRef> m_dataStoreConfiguration;
    Ref<WKWebsiteDataStoreRef> m_dataStore;
    Ref<WKContextConfigurationRef> m_contextConfiguration;
    Ref<WKContextRef> m_context;
    Ref<WKPreferencesRef> m_preferences;
    Ref<WKPageConfigurationRef> m_pageConfiguration;
};

} // namespace lumen
