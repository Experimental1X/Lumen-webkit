#include "download_transfer.h"

#include "downloads.h"
#include "util.h"

#include <winhttp.h>

#include <atomic>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace lumen {

namespace {

// Same family as kSafariUserAgent in webkit_host.cpp, but a separate
// constant: WinHTTP only reads this here and we deliberately do not look at
// or mutate that one. Mirrors don't demand an exact build string.
constexpr wchar_t kTransferUserAgent[] =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    L"AppleWebKit/602.1.50 (KHTML, like Gecko) "
    L"Version/18.4 Safari/602.1.50";

constexpr int kMaxRedirects = 8;
constexpr DWORD kReadChunk = 64 * 1024;

class HttpHandle {
public:
    explicit HttpHandle(HINTERNET handle) : m_handle(handle) {}
    ~HttpHandle() { if (m_handle) WinHttpCloseHandle(m_handle); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;

    HINTERNET get() const { return m_handle; }
    explicit operator bool() const { return m_handle != nullptr; }

private:
    HINTERNET m_handle;
};

// RAII for the destination file. Created with GENERIC_WRITE + CREATE_ALWAYS;
// closes the handle on any exit path (cancelled mid-transfer, redirect, error
// before writing started). Without this, a return from inside the read loop
// would leak the handle and lock the destination against later overwrites.
class FileHandle {
public:
    explicit FileHandle(HANDLE handle) : m_handle(handle) {}
    ~FileHandle() { if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    HANDLE get() const { return m_handle; }
    explicit operator bool() const { return m_handle != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_handle;
};

bool parseUrl(const std::wstring& url, std::wstring& host, std::wstring& path,
              INTERNET_SCHEME& scheme, INTERNET_PORT& port, std::wstring& error)
{
    URL_COMPONENTS components { };
    components.dwStructSize = sizeof(components);
    std::vector<wchar_t> hostBuffer(256);
    std::vector<wchar_t> pathBuffer(2048);
    components.lpszHostName = hostBuffer.data();
    components.dwHostNameLength = static_cast<DWORD>(hostBuffer.size());
    components.lpszUrlPath = pathBuffer.data();
    components.dwUrlPathLength = static_cast<DWORD>(pathBuffer.size());

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        error = L"WinHttpCrackUrl failed";
        return false;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"only HTTP/HTTPS are accepted (scheme index "
              + std::to_wstring(static_cast<int>(components.nScheme)) + L")";
        return false;
    }
    host.assign(hostBuffer.data(), components.dwHostNameLength);
    path.assign(pathBuffer.data(), components.dwUrlPathLength);
    scheme = components.nScheme;
    port = components.nPort;
    return true;
}

DWORD statusOf(HINTERNET request)
{
    DWORD status = 0;
    DWORD size = sizeof(status);
    // nullptr for the header name, not WINHTTP_HEADER_NAME: that macro is
    // NULL in this SDK, and with /permissive- the compiler reads the
    // expansion as a type in this argument position and rejects it
    // (C2275). nullptr is the same value spelled in a way that compiles.
    if (!WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            nullptr, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return 0;
    return status;
}

long long contentLengthOf(HINTERNET request)
{
    wchar_t value[64] { };
    DWORD size = sizeof(value);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
            nullptr, value, &size, WINHTTP_NO_HEADER_INDEX))
        return -1;
    return _wtoi64(value);
}

std::wstring headerOf(HINTERNET request, DWORD info)
{
    wchar_t value[2048] { };
    DWORD size = sizeof(value);
    if (!WinHttpQueryHeaders(request, info, nullptr,
            value, &size, WINHTTP_NO_HEADER_INDEX))
        return { };
    if (size > sizeof(value))
        size = sizeof(value);

    // WinHttpQueryHeaders does not agree with itself about whether the byte
    // count it reports includes the terminating null, and the two versions of
    // the header disagree between Windows releases. Build the string, then
    // strip any trailing nulls: correct either way.
    std::wstring result(value, size / sizeof(wchar_t));
    while (!result.empty() && result.back() == L'\0')
        result.pop_back();
    return result;
}

// Resolves a Location header against the URL it came from. Mirrors routinely
// redirect with a root-relative path ("/next/f9a9...") rather than an absolute
// URL, and handing that straight back to WinHttpCrackUrl fails - the scheme
// and host are missing. Only the two forms we actually see are handled:
// absolute (used as-is) and root-relative (scheme + host prepended).
std::wstring resolveLocation(const std::wstring& location, const std::wstring& base)
{
    if (location.empty())
        return { };
    if (location.find(L"://") != std::wstring::npos)
        return location;

    const size_t scheme = base.find(L"://");
    if (scheme == std::wstring::npos)
        return location;

    const size_t hostStart = scheme + 3;
    size_t hostEnd = base.find_first_of(L"/?#", hostStart);
    if (hostEnd == std::wstring::npos)
        hostEnd = base.size();

    std::wstring authority = base.substr(0, hostEnd);
    if (location[0] == L'/')
        return authority + location;

    // Path-relative: replace the last path segment of the base.
    const size_t lastSlash = base.find_last_of(L'/', hostEnd);
    const std::wstring directory = lastSlash == std::wstring::npos
        ? authority + L"/"
        : base.substr(0, lastSlash + 1);
    return directory + location;
}

} // namespace

void runWinhttpTransfer(unsigned id, std::wstring url)
{
    HttpHandle session(WinHttpOpen(kTransferUserAgent,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        Downloads::instance().reportFail(id, L"WinHttpOpen failed");
        return;
    }

    long long total = 0;
    std::wstring finalError;
    bool ok = false;

    for (int redirect = 0; redirect <= kMaxRedirects; ++redirect) {
        std::wstring host, path, error;
        INTERNET_SCHEME scheme;
        INTERNET_PORT port;
        if (!parseUrl(url, host, path, scheme, port, error)) {
            finalError = error;
            break;
        }

        HttpHandle connection(WinHttpConnect(session.get(), host.c_str(), port, 0));
        if (!connection) {
            finalError = L"WinHttpConnect failed for " + host;
            break;
        }

        const DWORD flags = (scheme == INTERNET_SCHEME_HTTPS)
            ? WINHTTP_FLAG_SECURE : 0;
        HttpHandle request(WinHttpOpenRequest(connection.get(),
            L"GET", path.c_str(), nullptr, nullptr, nullptr, flags));
        if (!request) {
            finalError = L"WinHttpOpenRequest failed";
            break;
        }

        // WinHttpOpen installed the user-agent on the session; WinHTTP sends it
        // on every request without us duplicating the header here.
        if (!WinHttpSendRequest(request.get(), nullptr, 0, nullptr, 0, 0, 0)) {
            finalError = L"WinHttpSendRequest failed";
            break;
        }
        // Two parameters: the request and a reserved LPVOID, which has been
        // unused since Windows 2000 and is always NULL. The one-argument
        // form people remember from the docs does not exist in the header.
        if (!WinHttpReceiveResponse(request.get(), nullptr)) {
            finalError = L"WinHttpReceiveResponse failed";
            break;
        }

        const DWORD status = statusOf(request.get());
        if (status >= 300 && status < 400) {
            // 304 Not Modified carries no body and no usable Location - treat
            // it as terminal rather than looping on an empty redirect.
            const std::wstring location = resolveLocation(
                headerOf(request.get(), WINHTTP_QUERY_LOCATION), url);
            if (location.empty() || redirect == kMaxRedirects) {
                finalError = L"redirect with no usable Location header ("
                           + std::to_wstring(static_cast<int>(status)) + L")";
                break;
            }
            log(L"Transfer redirect " + std::to_wstring(static_cast<int>(status))
                + L" for id " + std::to_wstring(id) + L": " + location);
            url = location;
            continue;
        }

        if (status >= 400) {
            finalError = L"HTTP " + std::to_wstring(static_cast<int>(status));
            break;
        }

        const long long headerLength = contentLengthOf(request.get());
        if (headerLength > 0)
            total = headerLength;

        // Tell the panel the total right away so the bar appears at its
        // final width before the first byte lands. reportProgress is the
        // single point that updates progress, so we go through it for the
        // initial 0/= total too.
        Downloads::instance().reportProgress(id, 0, total);

        const std::wstring destination = Downloads::instance().destinationFor(id);
        if (destination.empty()) {
            finalError = L"destination disappeared while id=" + std::to_wstring(id);
            break;
        }
        FileHandle file(CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) {
            finalError = L"cannot open destination for write (id="
                       + std::to_wstring(id) + L")";
            break;
        }

        long long received = 0;
        bool bodyOk = true;
        std::wstring bodyError;
        std::vector<char> buffer(kReadChunk);

        while (true) {
            // Cancellation check between chunks. The UI thread flips
            // item.state to Cancelled; we pick that up here and bail
            // before reading more data. Cheaper than writing a parallel
            // cancel map and lands the same on the wire: stops reading
            // and stops writing, the file is left as a truncated
            // artifact of an aborted transfer - same end state as
            // WKDownloadCancel. We early-return rather than fall through
            // to the reportFail path because that would overwrite the
            // Cancelled state with Failed and the panel would flash a
            // red failed row in place of "Cancelled".
            if (!Downloads::instance().stillActive(id))
                return;

            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.get(), &available)) {
                bodyError = L"WinHttpQueryDataAvailable failed";
                bodyOk = false;
                break;
            }
            if (available == 0)
                break; // end of body

            DWORD want = available < buffer.size()
                ? available : static_cast<DWORD>(buffer.size());
            DWORD read = 0;
            if (!WinHttpReadData(request.get(), buffer.data(), want, &read)
                || read == 0) {
                bodyError = L"WinHttpReadData failed";
                bodyOk = false;
                break;
            }

            DWORD written = 0;
            if (!WriteFile(file.get(), buffer.data(), read, &written, nullptr)
                || written != read) {
                bodyError = L"WriteFile failed";
                bodyOk = false;
                break;
            }

            received += read;
            // Updates flow through Downloads::reportProgress, which holds
            // m_lock and calls notify(false) once the value moves. A 64KB
            // chunk means roughly a couple of notifications per second for
            // a normal download; a saturated 1 Gbps link would push that
            // higher, but that's well under any UI-thread saturation
            // threshold and stays off the WebKit IPC path entirely.
            Downloads::instance().reportProgress(id, received, total);
        }

        // FileHandle's destructor closes the file when its scope ends.
        if (!bodyOk) {
            finalError = bodyError;
            break;
        }
        if (headerLength > 0 && received < headerLength) {
            finalError = L"connection closed at "
                       + std::to_wstring(received) + L" of "
                       + std::to_wstring(headerLength) + L" bytes";
            break;
        }

        ok = true;
        break;
    }

    if (ok) {
        Downloads::instance().reportFinish(id, total);
    } else {
        if (finalError.empty())
            finalError = L"unknown failure";
        Downloads::instance().reportFail(id, finalError);
    }
}

void startWinhttpTransfer(unsigned id, std::wstring url)
{
    std::thread([id, url = std::move(url)] {
        // Each transfer lives on its own detached thread; downloads parallel
        // to the UI thread and never runs anything that needs the UI
        // thread to be free. We deliberately do not join or store this
        // thread: it ends naturally when the body drains.
        runWinhttpTransfer(id, std::move(url));
    }).detach();
}

} // namespace lumen
