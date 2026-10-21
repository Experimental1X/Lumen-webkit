#include "downloads.h"

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
        // network progress; calling Browser code (which touches GDI, the panel
        // window, etc.) from there would be undefined behaviour, and WebKit's
        // didWriteData was being silently dropped in tests.
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

    {
        Lock lock(m_lock);
        for (auto& item : m_items) {
            if (item.state != DownloadItem::State::Active || item.path.empty())
                continue;

            WIN32_FILE_ATTRIBUTE_DATA data { };
            if (!GetFileAttributesExW(item.path.c_str(), GetFileExInfoStandard, &data))
                continue;

            const long long size = (static_cast<long long>(data.nFileSizeHigh) << 32)
                | static_cast<long long>(data.nFileSizeLow);

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
    }

    log(L"Download started.");
    notify(true);
}

WKStringRef Downloads::decideDestination(WKDownloadRef download, WKURLResponseRef response, WKStringRef suggestedFilename, const void*)
{
    Downloads& self = instance();

    std::wstring name = sanitizeFilename(toWide(suggestedFilename));
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
    {
        Lock lock(m_lock);
        DownloadItem* item = findIdLocked(id);
        if (!item || item->state != DownloadItem::State::Active || !item->ref)
            return;

        item->state = DownloadItem::State::Cancelled;
        target = item->ref;
    }

    WKDownloadCancel(target, nullptr, &cancelFinished);
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
