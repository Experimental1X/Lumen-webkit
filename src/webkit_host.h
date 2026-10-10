#pragma once

#include <windows.h>

#include <WebKit/WKContext.h>
#include <WebKit/WKContextConfigurationRef.h>
#include <WebKit/WKPage.h>
#include <WebKit/WKPageConfigurationRef.h>
#include <WebKit/WKPageStateClient.h>
#include <WebKit/WKPreferencesRef.h>
#include <WebKit/WKRetainPtr.h>
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

// One web view (one tab's worth of content). The browser UI owns these.
class WebView {
public:
    ~WebView();

    HWND hwnd() const;
    WKPageRef page() const;

    void load(const std::wstring& url);
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

    WKRetainPtr<WKViewRef> m_view;
    std::wstring m_title;
    std::wstring m_url;
};

// Owns the WebKit context shared by every view in the process.
class WebKitHost {
public:
    bool initialize(std::wstring& error);
    std::unique_ptr<WebView> createView(HWND parent);

private:
    WKRetainPtr<WKWebsiteDataStoreConfigurationRef> m_dataStoreConfiguration;
    WKRetainPtr<WKWebsiteDataStoreRef> m_dataStore;
    WKRetainPtr<WKContextConfigurationRef> m_contextConfiguration;
    WKRetainPtr<WKContextRef> m_context;
    WKRetainPtr<WKPreferencesRef> m_preferences;
    WKRetainPtr<WKPageConfigurationRef> m_pageConfiguration;
};

} // namespace lumen
