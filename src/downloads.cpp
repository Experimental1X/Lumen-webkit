#include "downloads.h"

#include "download_transfer.h"
#include "util.h"
#include "webkit_util.h"

#include <WebKit/WKDownloadClient.h>
#include <WebKit/WKDownloadRef.h>
#include <WebKit/WKURLRequest.h>
#include <WebKit/WKURLResponse.h>
#include <WebKit/WKType.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace lumen {

namespace {

std::wstring downloadsFolder()
{
    wchar_t buffer[MAX_PATH * 2] { };
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", buffer, static_cast<DWORD>(std::size(buffer)));

    std::wstring folder = (length > 0 && length < std::size(buffer))
        ? std::wstring(buffer, length) + L"\\Downloads"
        : executableDirectory();
    CreateDirectoryW(folder.c_str(), nullptr);
    return folder;
}

// Makes a name that Windows accepts as a file name.
std::wstring sanitizeFilename(const std::wstring& name)
{
    std::wstring result;
    for (wchar_t ch : name) {
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*')
            result += L'_';
        else
            result += ch;
    }

    while (!result.empty() && (result.back() == L' ' || result.back() == L'.'))
        result.pop_back();
    while (!result.empty() && result.front() == L' ')
        result.erase(result.begin());

    if (result.size() > 150) {
        const size_t dot = result.find_last_of(L'.');
        const std::wstring extension = (dot != std::wstring::npos && result.size() - dot <= 12) ? result.substr(dot) : L"";
        result = result.substr(0, 150 - extension.size()) + extension;
    }

    // Reserved device names such as CON or NUL.
    std::wstring stem = result.substr(0, result.find(L'.'));
    for (auto& ch : stem)
        ch = static_cast<wchar_t>(towupper(ch));
    static const wchar_t* const reserved[] = { L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"LPT1", L"LPT2", L"LPT3" };
    for (const wchar_t* blocked : reserved) {
        if (stem == blocked) {
            result = L"_" + result;
            break;
        }
    }

    return result;
}

bool pathExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// "report.pdf" becomes "report (1).pdf" when it already exists.
std::wstring uniquePath(const std::wstring& folder, const std::wstring& name)
{
    std::wstring candidate = folder + L"\\" + name;
    if (!pathExists(candidate))
        return candidate;

    const size_t dot = name.find_last_of(L'.');
    const std::wstring stem = dot != std::wstring::npos ? name.substr(0, dot) : name;
    const std::wstring extension = dot != std::wstring::npos ? name.substr(dot) : L"";

    for (int i = 1; i < 1000; ++i) {
        candidate = folder + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + extension;
        if (!pathExists(candidate))
            return candidate;
    }
    return folder + L"\\" + stem + L" (copy)" + extension;
}

std::wstring fileNameOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Every character is something a person would have typed, and there is at
// least one letter. This is what separates a description like "AOMEI
// Partition Assistant 10.12.2" from a name that has swallowed the opening
// bytes of a ZIP and come out as "PK☛☚AOMEI Partition...".
bool readsLikeWords(const std::wstring& name)
{
    if (name.empty() || name.size() > 120)
        return false;

    bool letter = false;
    for (wchar_t ch : name) {
        if (ch >= L'a' && ch <= L'z') { letter = true; continue; }
        if (ch >= L'A' && ch <= L'Z') { letter = true; continue; }
        if ((ch >= L'0' && ch <= L'9') || ch == L' ' || ch == L'.'
            || ch == L'-' || ch == L'_' || ch == L'(' || ch == L')')
            continue;
        return false;
    }
    return letter;
}

// A long run of hex digits with nothing a person would type in it. readsLikeWords
// accepts these happily - they are letters and numbers - but they are a content
// hash, not a filename, and using one gives you a Downloads folder full of
// "f9a9a647fda40a82.zip". FileCR hands out exactly that shape, so the check is
// worth having.
bool looksLikeHash(const std::wstring& name)
{
    // Long enough that no real product name of this shape exists, short enough
    // to catch the usual MD5/SHA prefixes.
    if (name.size() < 12)
        return false;

    // Ignore the extension before judging the stem.
    std::wstring stem = name;
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos)
        stem.erase(dot);

    if (stem.size() < 12)
        return false;

    bool hasHexDigit = false;
    for (wchar_t ch : stem) {
        const bool hex = (ch >= L'0' && ch <= L'9')
            || (ch >= L'a' && ch <= L'f')
            || (ch >= L'A' && ch <= L'F');
        if (!hex)
            return false;
        if (ch >= L'0' && ch <= L'9')
            hasHexDigit = true;
    }
    return hasHexDigit;
}

// Ends in something short enough after the last dot to be an extension.
bool hasShortExtension(const std::wstring& name)
{
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos)
        return false;

    const size_t tail = name.size() - dot - 1;
    if (tail < 1 || tail > 5)
        return false;

    for (size_t i = dot + 1; i < name.size(); ++i) {
        const wchar_t ch = name[i];
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z')
            || (ch >= L'0' && ch <= L'9')))
            return false;
    }
    return true;
}

// A name worth keeping as it stands: readable, and it says what it is.
bool isReadableName(const std::wstring& name)
{
    return readsLikeWords(name) && !looksLikeHash(name) && hasShortExtension(name);
}

// The name WebKit suggests comes from Content-Disposition, and when a server
// sends no filename header at all it falls back to the last path segment of
// the URL. Hosts that slug a file's own opening bytes into that path - FileCR
// and its mirrors do, precisely so every mirror derives one URL from the file
// alone - therefore hand us names like "PK☛☚AOMEI Partition Assistant":
// unprintable, no extension, useless in a Downloads folder.
//
// So walk the path back for the most recent segment that reads like words and
// use that instead.
std::wstring nameFromDownloadUrl(WKDownloadRef download, const std::wstring& fallback)
{
    std::wstring url;
    if (auto request = adopt(WKDownloadCopyRequest(download)))
        if (auto handle = adopt(WKURLRequestCopyURL(request.get())))
            url = toWide(handle.get());

    const size_t cut = url.find_first_of(L"?#");
    if (cut != std::wstring::npos)
        url.resize(cut);

    std::wstring best;
    size_t start = 0;
    while (start < url.size()) {
        const size_t slash = url.find(L'/', start);
        const std::wstring segment = percentDecode(
            slash == std::wstring::npos ? url.substr(start) : url.substr(start, slash - start));
        if (readsLikeWords(segment) && !looksLikeHash(segment))
            best = segment;

        if (slash == std::wstring::npos)
            break;
        start = slash + 1;
    }

    // The salvaged segment describes the file rather than naming it, and these
    // hosts only ever serve archives - so ".zip" is the guess worth making,
    // unless the segment already carries an extension of its own.
    // Finding nothing at all means the URL said nothing useful either.
    if (!best.empty())
        return hasShortExtension(best) ? sanitizeFilename(best)
                                       : sanitizeFilename(best) + L".zip";

    return sanitizeFilename(fallback);
}

// Same derivation as nameFromDownloadUrl, but takes a plain URL string
// instead of a WKDownloadRef. The WinHTTP path in download_transfer.cpp
// needs this because it never goes through decideDestination.
std::wstring nameFromUrlString(const std::wstring& url)
{
    std::wstring trimmed = url;
    const size_t cut = trimmed.find_first_of(L"?#");
    if (cut != std::wstring::npos)
        trimmed.resize(cut);

    std::wstring best;
    size_t start = 0;
    while (start < trimmed.size()) {
        const size_t slash = trimmed.find(L'/', start);
        const std::wstring segment = percentDecode(
            slash == std::wstring::npos ? trimmed.substr(start)
                                        : trimmed.substr(start, slash - start));
        if (readsLikeWords(segment) && !looksLikeHash(segment))
            best = segment;

        if (slash == std::wstring::npos)
            break;
        start = slash + 1;
    }

    if (best.empty())
        return { };
    return hasShortExtension(best) ? sanitizeFilename(best)
                                   : sanitizeFilename(best) + L".zip";
}

// Last-resort filename for a URL that had no human-readable segment. The
// WKDownload path's decideDestination uses Content-Disposition's suggested
// name as the fallback; this is the equivalent for the WinHTTP path that
// runs before any headers arrive, so the panel sees a name on row 1
// instead of "download".
std::wstring fallbackFilenameForUrl(const std::wstring& url)
{
    std::wstring trimmed = url;
    const size_t cut = trimmed.find_first_of(L"?#");
    if (cut != std::wstring::npos)
        trimmed.resize(cut);

    const size_t slash = trimmed.find_last_of(L'/');
    std::wstring segment = slash != std::wstring::npos
        ? percentDecode(trimmed.substr(slash + 1))
        : percentDecode(trimmed);

    if (segment.empty())
        return { };

    // Mirrors like s1-filecr.xyz/<hash> produce a hex-only segment. We let
    // it through instead of substituting "download" so a hash-only URL
    // still names itself accurately; sanitize it so trailing dots, reserved
    // device names, etc. never reach the file system.
    if (hasShortExtension(segment))
        return sanitizeFilename(segment);
    return sanitizeFilename(segment) + L".zip";
}

std::wstring hostFromUrl(std::wstring url)
{
    const size_t scheme = url.find(L"://");
    if (scheme != std::wstring::npos)
        url.erase(0, scheme + 3);
    const size_t end = url.find_first_of(L"/?#");
    if (end != std::wstring::npos)
        url.erase(end);
    if (url.rfind(L"www.", 0) == 0)
        url.erase(0, 4);
    return url;
}

void cancelFinished(WKDataRef, const void*)
{
}

// Scoped lock for m_lock. Every access to the download list and the client
// tables goes through one of these.
class Lock {
public:
    explicit Lock(CRITICAL_SECTION& section)
        : m_section(section)
    {
        EnterCriticalSection(&m_section);
    }
    ~Lock()
    {
        LeaveCriticalSection(&m_section);
    }

    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    CRITICAL_SECTION& m_section;
};

} // namespace

std::wstring formatBytes(long long bytes)
{
    if (bytes < 0)
        bytes = 0;

    const wchar_t* units[] = { L"bytes", L"KB", L"MB", L"GB", L"TB" };
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }

    wchar_t text[64] { };
    if (unit == 0)
        std::swprintf(text, std::size(text), L"%lld %ls", bytes, units[unit]);
    else
        std::swprintf(text, std::size(text), L"%.1f %ls", value, units[unit]);
    return text;
}

Downloads::Downloads()
{
    InitializeCriticalSection(&m_lock);
}

Downloads::~Downloads()
{
    DeleteCriticalSection(&m_lock);
}

Downloads& Downloads::instance()
{
    static Downloads downloads;
    return downloads;
}

DownloadItem* Downloads::findLocked(WKDownloadRef download)
{
    for (auto& item : m_items) {
        if (item.ref == download)
            return &item;
    }
    return nullptr;
}

DownloadItem* Downloads::findIdLocked(unsigned id)
{
    for (auto& item : m_items) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

std::vector<DownloadItem> Downloads::items() const
{
    Lock lock(m_lock);
    return m_items;
}

void Downloads::notify(bool started)
{
    if (m_ownerWindow) {
        // Pump the notification through the message loop onto the UI thread.
        // The download callbacks can fire on the IPC threads WebKit uses for
        // network progress; calling Browser code (which touches GDI, the
        // panel window, etc.) from there would be undefined behaviour.
        // WinHTTP workers on std::thread also talk to the UI through here,
        // for the same reason.
        PostMessageW(m_ownerWindow, WM_LUMEN_DOWNLOAD_CHANGED, started ? 1 : 0, 0);
    } else if (onChanged) {
        onChanged(started);
    }
}

bool Downloads::hasActive() const
{
    Lock lock(m_lock);
    for (const auto& item : m_items) {
        if (item.state == DownloadItem::State::Active)
            return true;
    }
    return false;
}

double Downloads::progress() const
{
    Lock lock(m_lock);
    long long received = 0;
    long long total = 0;
    for (const auto& item : m_items) {
        if (item.state == DownloadItem::State::Active && item.total > 0) {
            received += item.received;
            total += item.total;
        }
    }
    return total > 0 ? std::clamp(static_cast<double>(received) / static_cast<double>(total), 0.0, 1.0) : 0.0;
}

void Downloads::refreshFromDisk()
{
    bool changed = false;
    // One line per download, on the first poll that could measure it. This is
    // the whole reason a stalled download is diagnosable at all: "never
    // reached here" means the poll timer is not running, while "reached here
    // and the file is still 0 bytes" means WebKit is not writing to the
    // destination it gave us. Without the distinction both look identical from
    // the outside - a row that says 0 bytes for the entire download.
    bool loggedFirst = false;

    {
        Lock lock(m_lock);
        for (auto& item : m_items) {
            if (item.state != DownloadItem::State::Active)
                continue;

            if (item.path.empty()) {
                if (!loggedFirst) {
                    loggedFirst = true;
                    log(L"Download poll: no destination yet - decideDestination has not run.");
                }
                continue;
            }

            WIN32_FILE_ATTRIBUTE_DATA data { };
            if (!GetFileAttributesExW(item.path.c_str(), GetFileExInfoStandard, &data)) {
                if (!loggedFirst) {
                    loggedFirst = true;
                    log(L"Download poll: cannot stat " + item.path);
                }
                continue;
            }

            const long long size = (static_cast<long long>(data.nFileSizeHigh) << 32)
                | static_cast<long long>(data.nFileSizeLow);

            if (size == 0 && !loggedFirst) {
                loggedFirst = true;
                log(L"Download poll: " + item.path + L" exists and is still 0 bytes.");
            }

            if (size > item.received) {
                item.received = size;
                changed = true;
            }

            // A file that has reached its advertised size is done, whether or
            // not didFinish ever arrives. Leaving it Active is what made the
            // bar sit at a full width while the row still read "0 bytes".
            if (item.total > 0 && size >= item.total) {
                item.state = DownloadItem::State::Finished;
                item.ref = nullptr;
                changed = true;
                log(L"Download complete (measured on disk): " + item.path);
            }
        }
    }

    if (changed)
        notify(false);
}

void Downloads::attach(WKDownloadRef download)
{
    if (!download)
        return;

    WKRetain(download);

    {
        Lock lock(m_lock);
        if (findLocked(download)) {
            WKRelease(download);
            log(L"Download attach: already known, ignoring duplicate.");
            return;
        }

        DownloadItem item;
        item.id = m_nextId++;
        item.ref = download;
        item.filename = L"Download";

        auto request = adopt(WKDownloadCopyRequest(download));
        if (request) {
            auto url = adopt(WKURLRequestCopyURL(request.get()));
            item.source = hostFromUrl(toWide(url.get()));
        }

        // Read the host out before item is moved into the list below; after
        // the move it is a moved-from string and would log as empty.
        const std::wstring origin = item.source.empty() ? L"(unknown host)" : item.source;
        const int id = static_cast<int>(item.id);

        m_items.insert(m_items.begin(), std::move(item));

        // The callback table has to outlive this function: WKDownloadSetClient
        // keeps the pointer and reads it again on every later callback. See the
        // m_clients comment in downloads.h for what happens if it does not.
        auto client = std::make_unique<WKDownloadClientV0>();
        client->base.version = 0;
        client->base.clientInfo = this;
        client->decideDestinationWithResponse = &Downloads::decideDestination;
        client->didWriteData = &Downloads::didWriteData;
        client->didFinish = &Downloads::didFinish;
        client->didFailWithError = &Downloads::didFail;

        m_clients[download] = std::move(client);
        WKDownloadSetClient(download, &m_clients[download]->base);

        // Name the item here. A panel that opens with nothing in it, or a
        // download that never reaches decideDestination, is impossible to tell
        // apart from the log without knowing whether WebKit ever handed us one.
        log(L"Download attached: id=" + std::to_wstring(id)
            + L" from " + origin
            + L", " + std::to_wstring(m_items.size()) + L" in the list.");
    }

    notify(true);
}

void Downloads::startTransfer(std::wstring url)
{
    if (url.empty())
        return;

    // Derive a name from the URL path before opening the destination so the
    // panel sees the right row from frame one. decideDestination's
    // Content-Disposition suggestion is unavailable to us here, so the
    // chain is: human-readable path segment if there is one, else the URL's
    // final path segment (which may itself be a hash), else "download".
    std::wstring name = nameFromUrlString(url);
    if (name.empty())
        name = fallbackFilenameForUrl(url);
    if (name.empty())
        name = L"download";
    const std::wstring path = uniquePath(downloadsFolder(), name);
    const std::wstring filename = fileNameOf(path);
    const std::wstring host = hostFromUrl(url);

    unsigned id = 0;
    {
        Lock lock(m_lock);
        DownloadItem item;
        item.id = m_nextId++;
        item.ref = nullptr;
        item.filename = filename;
        item.path = path;
        item.source = host;
        item.received = 0;
        item.total = 0;
        item.state = DownloadItem::State::Active;
        id = item.id;
        m_items.insert(m_items.begin(), std::move(item));
    }

    log(L"Download created (WinHTTP): id=" + std::to_wstring(id)
        + L" from " + (host.empty() ? L"(unknown host)" : host)
        + L" -> " + path);
    notify(true);

    // Spawn the worker outside the lock; it will pull item.path through
    // destinationFor() and the byte counts through reportProgress().
    startWinhttpTransfer(id, std::move(url));
}

std::wstring Downloads::destinationFor(unsigned id) const
{
    Lock lock(m_lock);
    if (DownloadItem* item = findIdLocked(id))
        return item->path;
    return { };
}

bool Downloads::stillActive(unsigned id) const
{
    Lock lock(m_lock);
    if (DownloadItem* item = findIdLocked(id))
        return item->state == DownloadItem::State::Active;
    return false;
}

void Downloads::reportProgress(unsigned id, long long received, long long total)
{
    bool changed = false;
    {
        Lock lock(m_lock);
        if (DownloadItem* item = findIdLocked(id)) {
            if (received > item->received) {
                item->received = received;
                changed = true;
            }
            if (total > 0 && total != item->total) {
                item->total = total;
                changed = true;
            }
        }
    }
    if (changed)
        notify(false);
}

void Downloads::reportFinish(unsigned id, long long finalTotal)
{
    bool changed = false;
    std::wstring path;
    {
        Lock lock(m_lock);
        if (DownloadItem* item = findIdLocked(id)) {
            path = item->path;
            if (finalTotal > 0)
                item->total = finalTotal;
            item->state = DownloadItem::State::Finished;
            item->ref = nullptr;
            changed = true;
        }
    }
    if (changed) {
        log(L"Download finished (WinHTTP): " + path);
        notify(false);
    }
}

void Downloads::reportFail(unsigned id, std::wstring reason)
{
    bool changed = false;
    {
        Lock lock(m_lock);
        if (DownloadItem* item = findIdLocked(id)) {
            item->state = DownloadItem::State::Failed;
            item->ref = nullptr;
            changed = true;
        }
    }
    if (changed) {
        log(L"Download failed (WinHTTP): id=" + std::to_wstring(id)
            + L" " + reason);
        notify(false);
    }
}

WKStringRef Downloads::decideDestination(WKDownloadRef download, WKURLResponseRef response, WKStringRef suggestedFilename, const void*)
{
    Downloads& self = instance();

    std::wstring name = sanitizeFilename(toWide(suggestedFilename));
    if (!isReadableName(name))
        name = nameFromDownloadUrl(download, name);
    if (name.empty())
        name = L"download";

    const std::wstring path = uniquePath(downloadsFolder(), name);

    {
        Lock lock(self.m_lock);
        if (DownloadItem* item = self.findLocked(download)) {
            item->path = path;
            item->filename = fileNameOf(path);
            if (response) {
                const uint32_t expected = WKURLResponseGetExpectedContentLength(response);
                if (expected > 0)
                    item->total = expected;
            }
        }
    }

    log(L"Download destination: " + path);
    self.notify(false);

    // The string is returned with one reference that WebKit takes over. If it
    // only borrows it, the cost is a few bytes; releasing it here instead
    // could free it while WebKit still reads it.
    return WKStringCreateWithUTF8CString(toUtf8(path).c_str());
}

void Downloads::didWriteData(WKDownloadRef download, long long, long long totalBytesWritten, long long totalBytesExpectedToWrite, const void*)
{
    Downloads& self = instance();
    {
        Lock lock(self.m_lock);
        if (DownloadItem* item = self.findLocked(download)) {
            // Log the first chunk only, so lumen.log stays readable on a large
            // file but still proves the callback table was still alive.
            if (item->received == 0)
                log(L"Download first bytes received: " + item->filename);
            item->received = totalBytesWritten;
            if (totalBytesExpectedToWrite > 0)
                item->total = totalBytesExpectedToWrite;
        }
    }
    self.notify(false);
}

void Downloads::didFinish(WKDownloadRef download, const void*)
{
    Downloads& self = instance();
    {
        Lock lock(self.m_lock);
        if (DownloadItem* item = self.findLocked(download)) {
            item->state = DownloadItem::State::Finished;
            item->ref = nullptr;
            if (item->total <= 0)
                item->total = item->received;
            log(L"Download finished: " + item->path);
        }
        // didFinish is the last callback WebKit makes, so the table is free now.
        self.m_clients.erase(download);
    }
    self.notify(false);
    WKRelease(download);
}

void Downloads::didFail(WKDownloadRef download, WKErrorRef, WKDataRef, const void*)
{
    Downloads& self = instance();
    {
        Lock lock(self.m_lock);
        if (DownloadItem* item = self.findLocked(download)) {
            // Only report a failure for something still in flight. refreshFromDisk
            // marks a transfer complete the moment the file reaches its
            // advertised size, and a late didFail must not undo that.
            if (item->state == DownloadItem::State::Active)
                item->state = DownloadItem::State::Failed;
            item->ref = nullptr;
            log(L"Download did not complete: " + item->filename);
        }
        self.m_clients.erase(download);
    }
    self.notify(false);
    WKRelease(download);
}

void Downloads::cancel(unsigned id)
{
    WKDownloadRef target = nullptr;
    bool wasWinHttpTransfer = false;
    {
        Lock lock(m_lock);
        DownloadItem* item = findIdLocked(id);
        if (!item || item->state != DownloadItem::State::Active)
            return;

        // Two paths: WebKit-managed (item.ref is set) and our own WinHTTP
        // path (item.ref is null because the worker reads item.state under
        // stillActive()). For WebKit we need WKDownloadCancel; for WinHTTP
        // we just flag Cancelled and the worker aborts on its next chunk.
        if (item->ref)
            target = item->ref;
        else
            wasWinHttpTransfer = true;

        item->state = DownloadItem::State::Cancelled;
    }

    if (target)
        WKDownloadCancel(target, nullptr, &cancelFinished);
    else if (wasWinHttpTransfer)
        log(L"Download cancel marked (WinHTTP worker will stop at the next chunk): id="
            + std::to_wstring(id));

    notify(false);
}

void Downloads::remove(unsigned id)
{
    {
        Lock lock(m_lock);
        for (auto it = m_items.begin(); it != m_items.end(); ++it) {
            if (it->id != id)
                continue;

            // An active download is cancelled first; it is removed from the
            // list once WebKit has reported that it ended.
            if (it->state == DownloadItem::State::Active) {
                cancel(id);
                return;
            }

            m_items.erase(it);
            notify(false);
            return;
        }
    }
}

void Downloads::clearFinished()
{
    bool changed = false;
    {
        Lock lock(m_lock);
        for (auto it = m_items.begin(); it != m_items.end();) {
            if (it->state != DownloadItem::State::Active) {
                it = m_items.erase(it);
                changed = true;
            } else
                ++it;
        }
    }

    if (changed)
        notify(false);
}

} // namespace lumen
