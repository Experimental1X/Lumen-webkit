#pragma once

#include <windows.h>

#include <WebKit/WKBase.h>

#include <functional>
#include <string>
#include <vector>

namespace lumen {

struct DownloadItem {
    enum class State { Active, Finished, Failed, Cancelled };

    unsigned id { 0 };
    WKDownloadRef ref { nullptr }; // retained while the download is active
    std::wstring filename;
    std::wstring path;
    std::wstring source; // site the file came from
    long long received { 0 };
    long long total { 0 }; // 0 when the size is not known
    State state { State::Active };
};

// Keeps track of every download, whichever tab started it. Newest first.
class Downloads {
public:
    static Downloads& instance();

    // Starts tracking a download that WebKit created (from a link, a page
    // response or a context menu) and installs the callbacks for it.
    void attach(WKDownloadRef download);

    const std::vector<DownloadItem>& items() const { return m_items; }
    bool hasActive() const;
    // Combined progress (0..1) of the active downloads whose size is known.
    double progress() const;

    void cancel(unsigned id);
    void remove(unsigned id);
    void clearFinished();

    // Called on the UI thread when anything changes. `started` is true when a
    // new download has just begun.
    std::function<void(bool started)> onChanged;

private:
    DownloadItem* find(WKDownloadRef download);
    DownloadItem* findId(unsigned id);
    void notify(bool started);

    static WKStringRef decideDestination(WKDownloadRef download, WKURLResponseRef response, WKStringRef suggestedFilename, const void* clientInfo);
    static void didWriteData(WKDownloadRef download, long long bytesWritten, long long totalBytesWritten, long long totalBytesExpectedToWrite, const void* clientInfo);
    static void didFinish(WKDownloadRef download, const void* clientInfo);
    static void didFail(WKDownloadRef download, WKErrorRef error, WKDataRef resumeData, const void* clientInfo);

    std::vector<DownloadItem> m_items;
    unsigned m_nextId { 1 };
};

// "3.4 MB", "812 KB" ...
std::wstring formatBytes(long long bytes);

} // namespace lumen
