#include "webkit_host.h"

#include "util.h"

#include <algorithm>

namespace lumen {

namespace {

std::wstring toWide(WKStringRef string)
{
    if (!string)
        return { };

    const size_t maximumSize = WKStringGetMaximumUTF8CStringSize(string);
    if (!maximumSize)
        return { };

    std::string utf8(maximumSize, '\0');
    const size_t written = WKStringGetUTF8CString(string, utf8.data(), maximumSize);
    // The returned count includes the terminating NUL.
    utf8.resize(written > 0 ? written - 1 : 0);
    return fromUtf8(utf8);
}

std::wstring toWide(WKURLRef url)
{
    if (!url)
        return { };

    auto string = adopt(WKURLCopyString(url));
    return toWide(string.get());
}

Ref<WKURLRef> makeUrl(const std::wstring& value)
{
    const std::string utf8 = toUtf8(value);
    return adopt(WKURLCreateWithUTF8CString(utf8.c_str()));
}

} // namespace

// ---------------------------------------------------------------- WebView

WebView::~WebView()
{
    // Detach the callbacks without calling back into WebKit: any late
    // notification finds a null owner and does nothing.
    if (m_target)
        m_target->owner = nullptr;
    onChanged = nullptr;

    if (m_thumbnail)
        DeleteObject(m_thumbnail);
}

void WebView::setThumbnail(HBITMAP bitmap)
{
    if (m_thumbnail)
        DeleteObject(m_thumbnail);
    m_thumbnail = bitmap;
}

HWND WebView::hwnd() const
{
    return m_view ? WKViewGetWindow(m_view.get()) : nullptr;
}

WKPageRef WebView::page() const
{
    return m_view ? WKViewGetPage(m_view.get()) : nullptr;
}

void WebView::load(const std::wstring& address)
{
    auto* webPage = page();
    auto url = makeUrl(address);
    if (webPage && url)
        WKPageLoadURL(webPage, url.get());
}

void WebView::loadHtml(const std::string& utf8Html)
{
    auto* webPage = page();
    if (!webPage)
        return;

    auto html = adopt(WKStringCreateWithUTF8CString(utf8Html.c_str()));
    auto baseUrl = makeUrl(L"about:blank");
    WKPageLoadHTMLString(webPage, html.get(), baseUrl.get());
}

void WebView::setVisible(bool visible)
{
    if (!m_view)
        return;

    WKViewSetIsInWindow(m_view.get(), visible);
    if (HWND window = hwnd())
        ShowWindow(window, visible ? SW_SHOW : SW_HIDE);
}

void WebView::focus()
{
    if (HWND window = hwnd())
        SetFocus(window);
}

void WebView::setBackingScale(double scale)
{
    if (auto* webPage = page())
        WKPageSetCustomBackingScaleFactor(webPage, scale);
}

double WebView::zoom() const
{
    auto* webPage = page();
    return webPage ? WKPageGetPageZoomFactor(webPage) : 1.0;
}

void WebView::setZoom(double factor)
{
    if (auto* webPage = page())
        WKPageSetPageZoomFactor(webPage, std::clamp(factor, 0.3, 5.0));
}

void WebView::goBack()
{
    if (auto* webPage = page(); webPage && WKPageCanGoBack(webPage))
        WKPageGoBack(webPage);
}

void WebView::goForward()
{
    if (auto* webPage = page(); webPage && WKPageCanGoForward(webPage))
        WKPageGoForward(webPage);
}

void WebView::reloadOrStop()
{
    auto* webPage = page();
    if (!webPage)
        return;

    if (WKPageGetEstimatedProgress(webPage) < 1.0)
        WKPageStopLoading(webPage);
    else
        WKPageReload(webPage);
}

bool WebView::canGoBack() const
{
    auto* webPage = page();
    return webPage && WKPageCanGoBack(webPage);
}

bool WebView::canGoForward() const
{
    auto* webPage = page();
    return webPage && WKPageCanGoForward(webPage);
}

double WebView::progress() const
{
    auto* webPage = page();
    return webPage ? WKPageGetEstimatedProgress(webPage) : 1.0;
}

void WebView::refreshState()
{
    auto* webPage = page();
    if (!webPage)
        return;

    auto title = adopt(WKPageCopyTitle(webPage));
    auto url = adopt(WKPageCopyActiveURL(webPage));
    m_title = toWide(title.get());
    m_url = toWide(url.get());
}

void WebView::didChange(const void* clientInfo)
{
    auto* target = static_cast<const CallbackTarget*>(clientInfo);
    WebView* self = target ? target->owner : nullptr;
    if (self && self->onChanged)
        self->onChanged();
}

void WebView::didChangeTitleOrUrl(const void* clientInfo)
{
    auto* target = static_cast<const CallbackTarget*>(clientInfo);
    WebView* self = target ? target->owner : nullptr;
    if (!self)
        return;

    self->refreshState();
    if (self->onChanged)
        self->onChanged();
}

// ------------------------------------------------------------- WebKitHost

bool WebKitHost::initialize(std::wstring& error)
{
    // Experiment for the rendering glitches seen in the first build: ask
    // WebKit to paint on the CPU instead of the GPU. Helper processes inherit
    // this environment variable. Set LUMEN_GPU=1 before launching to skip it.
    if (!GetEnvironmentVariableW(L"LUMEN_GPU", nullptr, 0)) {
        SetEnvironmentVariableW(L"WEBKIT_SKIA_ENABLE_CPU_RENDERING", L"1");
        log(L"CPU rendering requested (set LUMEN_GPU=1 to disable).");
    }

    m_dataStoreConfiguration = adopt(WKWebsiteDataStoreConfigurationCreate());
    if (!m_dataStoreConfiguration) {
        error = L"Could not create the website data store configuration.";
        return false;
    }

    m_dataStore = adopt(WKWebsiteDataStoreCreateWithConfiguration(m_dataStoreConfiguration.get()));
    if (!m_dataStore) {
        error = L"Could not create the website data store.";
        return false;
    }

    m_contextConfiguration = adopt(WKContextConfigurationCreate());
    if (!m_contextConfiguration) {
        error = L"Could not create the WebKit context configuration.";
        return false;
    }

    m_context = adopt(WKContextCreateWithConfiguration(m_contextConfiguration.get()));
    if (!m_context) {
        error = L"Could not create the WebKit context.";
        return false;
    }

    m_preferences = adopt(WKPreferencesCreate());
    if (!m_preferences) {
        error = L"Could not create WebKit preferences.";
        return false;
    }

    m_pageConfiguration = adopt(WKPageConfigurationCreate());
    if (!m_pageConfiguration) {
        error = L"Could not create the page configuration.";
        return false;
    }

    WKPageConfigurationSetWebsiteDataStore(m_pageConfiguration.get(), m_dataStore.get());
    WKPageConfigurationSetContext(m_pageConfiguration.get(), m_context.get());
    WKPageConfigurationSetPreferences(m_pageConfiguration.get(), m_preferences.get());

    log(L"WebKit initialized.");
    return true;
}

std::unique_ptr<WebView> WebKitHost::createView(HWND parent)
{
    auto webView = std::make_unique<WebView>();

    RECT initialRect { };
    webView->m_view = adopt(WKViewCreate(initialRect, m_pageConfiguration.get(), parent));
    if (!webView->m_view) {
        log(L"WKViewCreate failed.");
        return nullptr;
    }

    WKViewSetIsInWindow(webView->m_view.get(), true);

    auto* webPage = webView->page();
    if (!webPage) {
        log(L"WKViewGetPage returned null.");
        return nullptr;
    }

    webView->m_target = new CallbackTarget { webView.get() };

    WKPageStateClientV0 client { };
    client.base.version = 0;
    client.base.clientInfo = webView->m_target;
    client.didChangeIsLoading = &WebView::didChange;
    client.didChangeTitle = &WebView::didChangeTitleOrUrl;
    client.didChangeActiveURL = &WebView::didChangeTitleOrUrl;
    client.didChangeEstimatedProgress = &WebView::didChange;
    client.didChangeCanGoBack = &WebView::didChange;
    client.didChangeCanGoForward = &WebView::didChange;
    WKPageSetPageStateClient(webPage, &client.base);

    const UINT dpi = GetDpiForWindow(parent);
    WKPageSetCustomBackingScaleFactor(webPage, dpi ? static_cast<double>(dpi) / 96.0 : 1.0);

    return webView;
}

} // namespace lumen
