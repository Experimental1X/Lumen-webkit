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

bool startsWith(const std::string& bytes, const char* signature, size_t length)
{
    return bytes.size() >= length && bytes.compare(0, length, signature, length) == 0;
}

// True when a path segment is really the first few bytes of a file rather
// than a name. Direct-download hosts increasingly slug the archive's own
// opening bytes into the path - FileCR's "Click to Download" hands you
// .../PK%E2%80%9A%E2%80%9D%E2%80%90AOMEI%20Partition%20... - because that way
// every mirror derives the same URL from the file alone. Nothing about that
// path says ".zip", so without this the archive lands in the tab and renders
// as text starting "PK".
bool looksLikeFileSignature(const std::string& bytes)
{
    static const struct { const char* magic; size_t length; } signatures[] = {
        { "PK\x03\x04", 4 },                       // zip
        { "PK\x05\x06", 4 },                       // zip, empty archive
        { "PK\x07\x08", 4 },                       // zip, spanned
        { "Rar!\x1a\x07", 7 },                     // rar
        { "7z\xbc\xaf\x27\x1c", 6 },               // 7z
        { "\x1f\x8b", 2 },                         // gzip
        { "\xd0\xcf\x11\xe0", 4 },                 // msi / ole compound file
        { "!<arch>\n", 7 },                        // ar, and therefore deb
        { "\x7f" "ELF", 4 },                       // linux binary
        { "%PDF", 4 },                             // pdf
    };

    for (const auto& signature : signatures)
        if (startsWith(bytes, signature.magic, signature.length))
            return true;

    // A DOS executable starts MZ too, but the header is sixty-four bytes long
    // and a page URL's last segment is never sixty-four bytes of binary that
    // starts with it - "MZP" for Mailbox Prime is the near miss worth ruling
    // out, and demanding the length does that without losing a real hit.
    return bytes.size() >= 64 && startsWith(bytes, "MZ", 2);
}

// Strips the query and fragment: neither is part of a file name.
std::wstring pathOf(const std::wstring& url)
{
    std::wstring path = url;
    const size_t fragment = path.find(L'#');
    if (fragment != std::wstring::npos)
        path.resize(fragment);
    const size_t query = path.find(L'?');
    if (query != std::wstring::npos)
        path.resize(query);
    return path;
}

std::wstring lowered(const std::wstring& text)
{
    std::wstring result = text;
    for (wchar_t& c : result)
        c = static_cast<wchar_t>(towlower(static_cast<int>(c)));
    return result;
}

// The last path segment ends with a known archive, package or installer
// extension.
bool hasBinaryExtension(const std::wstring& url)
{
    const std::wstring path = pathOf(url);
    const size_t slash = path.find_last_of(L'/');
    const std::wstring filename = lowered(slash == std::wstring::npos ? path : path.substr(slash + 1));
    if (filename.empty())
        return false;

    static const wchar_t* const binary[] = {
        L".zip", L".7z", L".rar", L".tar", L".gz", L".tgz", L".bz2",
        L".xz", L".lz", L".lzma", L".zst",
        L".iso", L".img", L".dmg", L".bin", L".cue",
        L".exe", L".msi", L".msp", L".dll", L".cab", L".scr",
        L".deb", L".rpm", L".pkg", L".apk", L".ipa",
        L".jar", L".war",
        L".pdf",
    };
    for (const wchar_t* ext : binary)
        if (filename.size() >= std::wcslen(ext)
            && filename.compare(filename.size() - std::wcslen(ext), std::wcslen(ext), ext) == 0)
            return true;

    return false;
}

// The last path segment has no extension, but it is the head of a real file -
// see looksLikeFileSignature for why that happens.
bool hasFileSignature(const std::wstring& url)
{
    const std::wstring path = pathOf(url);
    const size_t slash = path.find_last_of(L'/');
    if (slash == std::wstring::npos)
        return false;

    return looksLikeFileSignature(percentDecodeBytes(path.substr(slash + 1)));
}

// Strong signals only: the URL positively says this is a file, with no
// guessing. isDownloadUrl layers looser path markers on top; keeping the two
// apart makes it obvious which rule actually caught any given download.
bool isFileUrl(const std::wstring& url)
{
    if (hasBinaryExtension(url))
        return true;

    // FileCR's per-mirror hosts serve nothing but file payloads, and their
    // paths carry no extension. Keyed on the mirror domain rather than the
    // main site, because filecr.com itself is an ordinary set of pages.
    if (lowered(url).find(L"filecr.xyz") != std::wstring::npos)
        return true;

    return hasFileSignature(url);
}

// URL-by-extension companion for download sniffing in decideNavigationAction.
// Most direct-download links (GitHub Releases, SourceForge mirrors, FileCR,
// macOS / Linux ISOs, browser installers) never send Content-Disposition:
// attachment, so keying off that header alone misses them. The URL is
// reachable from the action callback, so we check it there.
//
// Deliberately narrow: media files (.mp3, .mp4, etc.) are excluded so a
// link on a music page does not turn into a download.
bool isDownloadUrl(const std::wstring& url)
{
    if (isFileUrl(url))
        return true;

    // Many file hosts and CDNs use a path like /download/abc/setup.exe
    // where the path itself advertises the intent even if there is no
    // trailing extension. Sniff those patterns.
    const std::wstring path = lowered(pathOf(url));
    static const wchar_t* const markers[] = {
        L"/download/", L"/downloads/", L"/getfile/", L"/get/", L"/file/",
        L"/releases/download/", L"/releases/tag/",
    };
    for (const wchar_t* marker : markers)
        if (path.find(marker) != std::wstring::npos)
            return true;

    return false;
}

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
    std::wstring wurl;
    auto request = adopt(WKNavigationActionCopyRequest(action));
    if (request) {
        auto url = adopt(WKURLRequestCopyURL(request.get()));
        if (url) {
            auto urlString = adopt(WKURLCopyString(url.get()));
            wurl = toWide(urlString.get());
            if (wurl.rfind(L"lumen:", 0) == 0) {
                handleLumenAction(wurl);
                WKFramePolicyListenerIgnore(listener);
                return;
            }
        }
    }

    // Direct navigation to a binary file extension gets turned into a download
    // before any response is fetched. Most direct-download links (GitHub
    // Releases, SourceForge, the various macOS / Linux mirror sites) never
    // send Content-Disposition: attachment, so the attachment check alone
    // missed them and the bytes got rendered into the tab as mojibake
    // beginning "MZ" or "PK".
    //
    // This is now the only place a download is recognised, so say so in the
    // log. If a download does not appear, "Download attached" is missing from
    // the log while these lines are not, which says the URL never reached this
    // function at all; the opposite says the rule fired and the failure is
    // somewhere inside WebKit's own download machinery.
    if (!wurl.empty() && isDownloadUrl(wurl)) {
        log(L"Download rule matched on URL: " + wurl);
        WKFramePolicyListenerDownload(listener);
        return;
    }

    // <a download> and similar requests become downloads; the rest load.
    if (WKNavigationActionShouldPerformDownload(action)) {
        log(L"Download rule matched on the navigation action: " + wurl);
        WKFramePolicyListenerDownload(listener);
    } else
        WKFramePolicyListenerUse(listener);
}

void WebView::decideNavigationResponse(WKPageRef, WKNavigationResponseRef, WKFramePolicyListenerRef listener, WKTypeRef, const void*)
{
    // This used to re-check the URL as a second line of defence, on the theory
    // that a redirect into a file never comes back through
    // decideNavigationAction. It is gone, and the reason is worth recording so
    // nobody spends another build cycle rediscovering it: this SDK offers no way
    // to read the URL off a WKNavigationResponseRef. WKNavigationResponseGetURL
    // is not declared, and neither is WKPageGetProvisionalURL - the page
    // accessors that survive in this port are WKPageGetPageZoomFactor and
    // WKPageGetEstimatedProgress and little else. The correct signal, reading
    // Content-Type or Content-Disposition, is out of reach for the same reason:
    // no WKURLResponseGetMIMEType, no WKNavigationResponseIsForMainFrame, and
    // no way to get a WKURLResponseRef out of a WKNavigationResponseRef at all.
    //
    // So the action stage carries downloads on its own. That is not much of a
    // loss, because the links that matter are clicked at their final URL: the
    // FileCR "Click to Download" button navigates straight to
    // s1-filecr.xyz/.../PK%E2%80%9A..., so decideNavigationAction already sees
    // the file and isFileUrl already matches it on the mirror host and on the
    // percent-decoded PK signature.
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
