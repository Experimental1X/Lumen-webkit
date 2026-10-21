#include "webkit_host.h"

#include "settings.h"
#include "util.h"
#include "webkit_util.h"

#include "dialogs.h"
#include "downloads.h"

#include <WebKit/WKArray.h>
#include <WebKit/WKDownloadRef.h>
#include <WebKit/WKFindOptions.h>
#include <WebKit/WKFramePolicyListener.h>
#include <WebKit/WKNavigationActionRef.h>
#include <WebKit/WKNavigationResponseRef.h>
#include <WebKit/WKOpenPanelParametersRef.h>
#include <WebKit/WKOpenPanelResultListener.h>
#include <WebKit/WKPageFindClient.h>
#include <WebKit/WKPageNavigationClient.h>
#include <WebKit/WKPageUIClient.h>
#include <WebKit/WKPreferencesRefPrivate.h>
#include <WebKit/WKURLRequest.h>
#include <WebKit/WKURLResponse.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <shellapi.h>

namespace lumen {

namespace {

// MIME types that should be saved rather than rendered.
//
// The WinCairo port has no plug-in for archives or executables, so when a
// navigation to one is allowed through, WebKit falls back to showing the bytes
// as text: opening a .zip navigates the tab to a page of mojibake beginning
// "MZ" or "PK". Most direct-download links never send
// Content-Disposition: attachment, so keying off that header alone is what let
// those navigations through.
//
// Deliberately excludes the types a browser genuinely handles inline - text/*,
// images, audio, video and application/pdf.
bool isDownloadMimeType(const char* mime)
{
    if (!mime || !*mime)
        return false;

    // "text/plain; charset=utf-8" -> "text/plain", folded to lower case.
    char normalized[128] { };
    size_t length = 0;
    for (const char* p = mime; *p && *p != ';' && length + 1 < std::size(normalized); ++p) {
        if (*p == ' ')
            continue;
        normalized[length++] = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    }

    static const char* const blocked[] = {
        "application/zip",
        "application/x-zip",
        "application/x-zip-compressed",
        "application/octet-stream",
        "application/gzip",
        "application/x-gzip",
        "application/x-tar",
        "application/x-bzip2",
        "application/x-7z-compressed",
        "application/x-rar-compressed",
        "application/vnd.rar",
        "application/x-msdownload",
        "application/x-msdos-program",
        "application/x-msi",
        "application/vnd.microsoft.portable-executable",
        "application/x-executable",
        "application/x-elf",
        "application/x-iso9660-image",
        "application/x-cd-image",
        "application/x-apple-diskimage",
        "application/vnd.android.package-archive",
        "application/x-debian-package",
        "application/java-archive",
        "application/x-sqlite3",
        "application/vnd.ms-cab-compressed",
        "application/x-font-ttf",
        "application/vnd.ms-fontobject",
    };

    for (const char* candidate : blocked) {
        if (std::strcmp(normalized, candidate) == 0)
            return true;
    }
    return false;
}

} // namespace

namespace {

Ref<WKURLRef> makeUrl(const std::wstring& value)
{
    const std::string utf8 = toUtf8(value);
    return adopt(WKURLCreateWithUTF8CString(utf8.c_str()));
}

// Modern Safari on macOS. YouTube (and a handful of other sites) only unlock
// HD streams when the UA is one its player recognises as a current Safari
// on macOS; the default WebKit-on-Windows UA is capped at 360p.
constexpr wchar_t kSafariUserAgent[] =
    L"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
    L"(KHTML, like Gecko) Version/26.0 Safari/605.1.15";

// Internal lumen: URLs power the action buttons in the downloads page
// (lumen:cancel?id=N, lumen:open?id=N, lumen:reveal?id=N, lumen:remove?id=N,
// lumen:clear). They never actually load; the navigation is cancelled and
// the command is dispatched to the Downloads singleton.
void handleLumenAction(const std::wstring& url)
{
    if (url.rfind(L"lumen:", 0) != 0)
        return;

    const size_t colon = 5; // "lumen" is five characters; the colon follows.
    const size_t q = url.find(L'?', colon + 1);
    const std::wstring action = url.substr(colon + 1, q == std::wstring::npos ? std::wstring::npos : q - colon - 1);
    const std::wstring query = q == std::wstring::npos ? std::wstring() : url.substr(q + 1);

    auto getParam = [&](const std::wstring& key) -> std::wstring {
        const std::wstring prefix = key + L"=";
        size_t pos = query.find(prefix);
        if (pos == std::wstring::npos)
            return {};
        pos += prefix.size();
        const size_t end = query.find(L'&', pos);
        return query.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
    };

    auto findItem = [](unsigned id) -> const DownloadItem* {
        for (const auto& item : Downloads::instance().items()) {
            if (item.id == id)
                return &item;
        }
        return nullptr;
    };

    Downloads& downloads = Downloads::instance();

    if (action == L"cancel") {
        const unsigned id = static_cast<unsigned>(std::wcstoul(getParam(L"id").c_str(), nullptr, 10));
        downloads.cancel(id);
    } else if (action == L"remove") {
        const unsigned id = static_cast<unsigned>(std::wcstoul(getParam(L"id").c_str(), nullptr, 10));
        downloads.remove(id);
    } else if (action == L"open") {
        const unsigned id = static_cast<unsigned>(std::wcstoul(getParam(L"id").c_str(), nullptr, 10));
        if (const DownloadItem* item = findItem(id); item && item->state == DownloadItem::State::Finished)
            ShellExecuteW(nullptr, L"open", item->path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (action == L"reveal") {
        const unsigned id = static_cast<unsigned>(std::wcstoul(getParam(L"id").c_str(), nullptr, 10));
        if (const DownloadItem* item = findItem(id); item && !item->path.empty()) {
            const std::wstring arguments = L"/select,\"" + item->path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
        }
    } else if (action == L"clear") {
        downloads.clearFinished();
    } else if (action == L"settings") {
        // Either a value change (`?key=X&value=Y`), a pane change
        // (`?view=general`) or a one-shot action (`?key=__action__&value=…`).
        // Browser picks up the message below and re-renders the open Settings
        // tab so the new value shows up.
        const std::wstring key = getParam(L"key");
        const std::wstring value = getParam(L"value");
        const std::wstring view = getParam(L"view");

        HWND window = FindWindowW(L"LumenMainWindow", nullptr);

        if (!view.empty())
            Settings::instance().setView(view);
        else if (key == L"__action__" && value == L"clear_history") {
            // The Library is owned by Browser, so hand the work over rather
            // than touching its files from here.
            if (window)
                PostMessageW(window, WM_LUMEN_SETTINGS_CLEAR_HISTORY, 0, 0);
        } else if (!key.empty())
            Settings::instance().set(key, value);

        if (window)
            PostMessageW(window, WM_LUMEN_SETTINGS_CHANGED, 0, 0);
    }
}

} // namespace

// ---------------------------------------------------------------- WebView

WebView::~WebView()
{
    // Disconnect callbacks before any WebKit state goes away. A late
    // notification then finds `owner == nullptr` and is a no-op.
    if (m_target)
        m_target->owner = nullptr;
    onChanged = nullptr;
    onNewPage = nullptr;
    onCloseRequested = nullptr;

    // Do NOT call closePage() here: the tab's owner already called it before
    // stashing the view in m_closedTabs. A second call is a no-op anyway
    // because m_closed is set.

    // Always leak the WKViewRef. After WKPageClose has marked the page for
    // shutdown on this WinCairo build, releasing the WKView from the main
    // process side while the helper process is still wrapping up trips an
    // access violation at a fixed offset inside WebKit2.dll
    // (the +0x6C7606 crash in lumen.log). The WKView is freed when the
    // process exits; closing a tab costs ~one WKView's worth of memory
    // until then. This replaces the conditional leak that only fired when
    // WKPageClose wasn't exported.
    m_view.leak();
    m_leakOnDestroy = true;

    if (m_thumbnail)
        DeleteObject(m_thumbnail);
}

void WebView::closePage()
{
    if (m_closed)
        return;
    m_closed = true;

    auto* webPage = page();
    if (!webPage)
        return;

    // WKPageClose is looked up at run time, so a WebKit build without it
    // cannot break linking; in that case the view is leaked instead.
    using PageCloseFunction = void (*)(WKPageRef);
    static const auto pageClose = reinterpret_cast<PageCloseFunction>(
        GetProcAddress(GetModuleHandleW(L"WebKit2.dll"), "WKPageClose"));

    if (pageClose) {
        pageClose(webPage);
        log(L"WKPageClose called.");
    } else {
        m_leakOnDestroy = true;
        log(L"WKPageClose is not exported by WebKit2.dll; the view will be leaked instead of released.");
    }
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
    const std::wstring newTitle = toWide(title.get());
    const std::wstring newUrl = toWide(url.get());

    // When the address changes before the new page's title arrives, WebKit
    // still reports the old title. Do not attach it to the new address.
    m_title = (newUrl != m_url && newTitle == m_title) ? std::wstring() : newTitle;
    m_url = newUrl;
}

WebView* WebView::ownerOf(const void* clientInfo)
{
    auto* target = static_cast<const CallbackTarget*>(clientInfo);
    return target ? target->owner : nullptr;
}

HWND WebView::windowOf(WebView* view)
{
    HWND window = view ? view->hwnd() : nullptr;
    return window ? GetAncestor(window, GA_ROOT) : nullptr;
}

void WebView::didChange(const void* clientInfo)
{
    WebView* self = ownerOf(clientInfo);
    if (self && self->onChanged)
        self->onChanged();
}

void WebView::didChangeTitleOrUrl(const void* clientInfo)
{
    WebView* self = ownerOf(clientInfo);
    if (!self)
        return;

    self->refreshState();
    if (self->onChanged)
        self->onChanged();
}

// ---------------------------------------------------------- downloads

void WebView::decideNavigationAction(WKPageRef, WKNavigationActionRef action, WKFramePolicyListenerRef listener, WKTypeRef, const void*)
{
    // lumen: URLs are caught here so the downloads page buttons can dispatch
    // commands without round-tripping through real navigation.
    auto request = adopt(WKNavigationActionCopyRequest(action));
    if (request) {
        auto url = adopt(WKURLRequestCopyURL(request.get()));
        if (url) {
            auto urlString = adopt(WKURLCopyString(url.get()));
            const std::wstring wurl = toWide(urlString.get());
            if (wurl.rfind(L"lumen:", 0) == 0) {
                handleLumenAction(wurl);
                WKFramePolicyListenerIgnore(listener);
                return;
            }
        }
    }

    // <a download> and similar requests become downloads; the rest load.
    if (WKNavigationActionShouldPerformDownload(action))
        WKFramePolicyListenerDownload(listener);
    else
        WKFramePolicyListenerUse(listener);
}

void WebView::decideNavigationResponse(WKPageRef, WKNavigationResponseRef response, WKFramePolicyListenerRef listener, WKTypeRef, const void*)
{
    // A subframe is part of the page. An ad frame or an embedded viewer
    // fetching its own payload must never spawn a download, which is what
    // happened before this check existed - one YouTube page load produced two
    // phantom entries.
    if (!WKNavigationResponseIsForMainFrame(response)) {
        WKFramePolicyListenerUse(listener);
        return;
    }

    bool download = false;
    auto urlResponse = adopt(WKNavigationResponseCopyResponse(response));
    if (urlResponse) {
        // Content-Disposition: attachment is the unambiguous case, but most
        // direct-download links never send it - they just serve the bytes.
        // Those navigations used to be allowed through, which rendered the file
        // into the tab as a page of mojibake instead of saving it.
        download = WKURLResponseIsAttachment(urlResponse.get())
            || isDownloadMimeType(WKURLResponseGetMIMEType(urlResponse.get()));
    }

    if (download)
        WKFramePolicyListenerDownload(listener);
    else
        WKFramePolicyListenerUse(listener);
}

void WebView::actionBecameDownload(WKPageRef, WKNavigationActionRef, WKDownloadRef download, const void*)
{
    Downloads::instance().attach(download);
}

void WebView::responseBecameDownload(WKPageRef, WKNavigationResponseRef, WKDownloadRef download, const void*)
{
    Downloads::instance().attach(download);
}

void WebView::contextMenuCreatedDownload(WKPageRef, WKDownloadRef download, const void*)
{
    Downloads::instance().attach(download);
}

// ------------------------------------------------- new windows, dialogs

WKPageRef WebView::createNewPage(WKPageRef, WKPageConfigurationRef configuration, WKNavigationActionRef, WKWindowFeaturesRef, const void* clientInfo)
{
    WebView* self = ownerOf(clientInfo);
    if (!self || !self->onNewPage)
        return nullptr;

    WKPageRef newPage = self->onNewPage(configuration);
    if (newPage) {
        // The caller takes over one reference to the page. If WebKit instead
        // only borrows it, the cost is one page object that stays alive.
        WKRetain(newPage);
    }
    return newPage;
}

void WebView::closeRequested(WKPageRef, const void* clientInfo)
{
    WebView* self = ownerOf(clientInfo);
    if (self && self->onCloseRequested)
        self->onCloseRequested();
}

void WebView::runAlert(WKPageRef, WKStringRef text, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptAlertResultListenerRef listener, const void* clientInfo)
{
    showAlert(windowOf(ownerOf(clientInfo)), toWide(text));
    WKPageRunJavaScriptAlertResultListenerCall(listener);
}

void WebView::runConfirm(WKPageRef, WKStringRef text, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptConfirmResultListenerRef listener, const void* clientInfo)
{
    const bool accepted = showConfirm(windowOf(ownerOf(clientInfo)), toWide(text));
    WKPageRunJavaScriptConfirmResultListenerCall(listener, accepted);
}

void WebView::runPrompt(WKPageRef, WKStringRef text, WKStringRef defaultValue, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptPromptResultListenerRef listener, const void* clientInfo)
{
    std::wstring result;
    if (showPrompt(windowOf(ownerOf(clientInfo)), toWide(text), toWide(defaultValue), result)) {
        auto string = adopt(WKStringCreateWithUTF8CString(toUtf8(result).c_str()));
        WKPageRunJavaScriptPromptResultListenerCall(listener, string.get());
    } else
        WKPageRunJavaScriptPromptResultListenerCall(listener, nullptr);
}

void WebView::runOpenPanel(WKPageRef, WKFrameRef, WKOpenPanelParametersRef parameters, WKOpenPanelResultListenerRef listener, const void* clientInfo)
{
    const bool multiple = WKOpenPanelParametersGetAllowsMultipleFiles(parameters);
    const std::vector<std::wstring> files = chooseFiles(windowOf(ownerOf(clientInfo)), multiple);
    if (files.empty()) {
        WKOpenPanelResultListenerCancel(listener);
        return;
    }

    std::vector<Ref<WKURLRef>> urls;
    std::vector<WKTypeRef> values;
    for (const auto& file : files) {
        urls.push_back(adopt(WKURLCreateWithUTF8CString(toUtf8(fileUrlFromPath(file)).c_str())));
        values.push_back(urls.back().get());
    }

    auto array = adopt(WKArrayCreate(values.data(), values.size()));
    auto mimeTypes = adopt(WKArrayCreate(nullptr, 0));
    WKOpenPanelResultListenerChooseFiles(listener, array.get(), mimeTypes.get());
}

// ------------------------------------------------------------ find in page

void WebView::didFindString(WKPageRef, WKStringRef, unsigned matchCount, const void* clientInfo)
{
    WebView* self = ownerOf(clientInfo);
    if (!self)
        return;

    // Counting reports the real total; a single find reports at least one.
    self->m_findMatches = std::max(self->m_findMatches, static_cast<int>(matchCount));
    if (self->onChanged)
        self->onChanged();
}

void WebView::didFailToFindString(WKPageRef, WKStringRef, const void* clientInfo)
{
    WebView* self = ownerOf(clientInfo);
    if (!self)
        return;

    self->m_findMatches = 0;
    if (self->onChanged)
        self->onChanged();
}

void WebView::didCountStringMatches(WKPageRef, WKStringRef, unsigned matchCount, const void* clientInfo)
{
    WebView* self = ownerOf(clientInfo);
    if (!self)
        return;

    self->m_findMatches = static_cast<int>(matchCount);
    if (self->onChanged)
        self->onChanged();
}

void WebView::find(const std::wstring& text, bool backwards, bool countMatches)
{
    auto* webPage = page();
    if (!webPage)
        return;

    if (text.empty()) {
        hideFind();
        return;
    }

    auto string = adopt(WKStringCreateWithUTF8CString(toUtf8(text).c_str()));

    WKFindOptions options = kWKFindOptionsCaseInsensitive | kWKFindOptionsWrapAround
        | kWKFindOptionsShowFindIndicator | kWKFindOptionsShowHighlight;
    if (backwards)
        options |= kWKFindOptionsBackwards;

    if (countMatches)
        m_findMatches = -1;

    WKPageFindString(webPage, string.get(), options, 1000);
    if (countMatches)
        WKPageCountStringMatches(webPage, string.get(), options & ~static_cast<WKFindOptions>(kWKFindOptionsBackwards), 1000);
}

void WebView::hideFind()
{
    m_findMatches = -1;
    if (auto* webPage = page())
        WKPageHideFindUI(webPage);
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

    // YouTube only offers resolutions above 360p to browsers with Media
    // Source support. Record the default (to learn whether this build has it
    // switched off) and enable it.
    log(std::wstring(L"MediaSource enabled by default: ") + (WKPreferencesGetMediaSourceEnabled(m_preferences.get()) ? L"yes" : L"no"));
    WKPreferencesSetMediaSourceEnabled(m_preferences.get(), true);
    WKPreferencesSetMediaCapabilitiesEnabled(m_preferences.get(), true);
    WKPreferencesSetWebGLEnabled(m_preferences.get(), true);

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

std::unique_ptr<WebView> WebKitHost::createView(HWND parent, WKPageConfigurationRef configuration)
{
    auto webView = std::make_unique<WebView>();

    RECT initialRect { };
    WKPageConfigurationRef pageConfiguration = configuration ? configuration : m_pageConfiguration.get();
    webView->m_view = adopt(WKViewCreate(initialRect, pageConfiguration, parent));
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

    WKPageNavigationClientV3 navigation { };
    navigation.base.version = 3;
    navigation.base.clientInfo = webView->m_target;
    navigation.decidePolicyForNavigationAction = &WebView::decideNavigationAction;
    navigation.decidePolicyForNavigationResponse = &WebView::decideNavigationResponse;
    navigation.navigationActionDidBecomeDownload = &WebView::actionBecameDownload;
    navigation.navigationResponseDidBecomeDownload = &WebView::responseBecameDownload;
    navigation.contextMenuDidCreateDownload = &WebView::contextMenuCreatedDownload;
    WKPageSetPageNavigationClient(webPage, &navigation.base);

    WKPageUIClientV19 ui { };
    ui.base.version = 19;
    ui.base.clientInfo = webView->m_target;
    ui.createNewPage = &WebView::createNewPage;
    ui.close = &WebView::closeRequested;
    ui.runJavaScriptAlert = &WebView::runAlert;
    ui.runJavaScriptConfirm = &WebView::runConfirm;
    ui.runJavaScriptPrompt = &WebView::runPrompt;
    ui.runOpenPanel = &WebView::runOpenPanel;
    WKPageSetPageUIClient(webPage, &ui.base);

    WKPageFindClientV0 findClient { };
    findClient.base.version = 0;
    findClient.base.clientInfo = webView->m_target;
    findClient.didFindString = &WebView::didFindString;
    findClient.didFailToFindString = &WebView::didFailToFindString;
    findClient.didCountStringMatches = &WebView::didCountStringMatches;
    WKPageSetPageFindClient(webPage, &findClient.base);

    // Set the user agent to current Safari/macOS so YouTube serves HD
    // streams instead of capping at 360p.
    auto userAgent = adopt(WKStringCreateWithUTF8CString(toUtf8(kSafariUserAgent).c_str()));
    WKPageSetCustomUserAgent(webPage, userAgent.get());
    log(L"Custom user-agent set.");

    const UINT dpi = GetDpiForWindow(parent);
    WKPageSetCustomBackingScaleFactor(webPage, dpi ? static_cast<double>(dpi) / 96.0 : 1.0);

    return webView;
}

} // namespace lumen
