#pragma once

#include <windows.h>

#include <WebKit/WKBase.h>
#include <WebKit/WKDownloadClient.h>

#include <functional>
#include <map>
#include <memory>
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

// Custom message posted to the browser window when download state changes.
// The download callbacks can fire on the IPC threads WebKit uses for network
// progress; calling Browser code (which touches GDI, the panel window, etc.)
// from there would be undefined behaviour. Posting a message turns every
// notification into a UI-thread call. wParam is 1 if a new download just
// started, 0 otherwise.
#define WM_LUMEN_DOWNLOAD_CHANGED (WM_APP + 7)

// Keeps track of every download, whichever tab started it. Newest first.
class Downloads {
public:
    Downloads();
    ~Downloads();

    Downloads(const Downloads&) = delete;
    Downloads& operator=(const Downloads&) = delete;

    static Downloads& instance();

    // Starts tracking a download that WebKit created (from a link, a page
    // response or a context menu) and installs the callbacks for it.
    void attach(WKDownloadRef download);

    // Routes a URL through the WinHTTP path. Inside the lock we build a
    // DownloadItem with the chosen filename and a destination file, then
    // hand the URL + id to download_transfer.cpp. The caller (the
    // navigation policy handler in webkit_host.cpp) cancels the WebKit
    // navigation first so the URL only flows once.
    void startTransfer(std::wstring url);

    // Updates an item from a worker thread. Progress is rate-limited: only
    // writes and notifies if the byte count moved or the total changed.
    void reportProgress(unsigned id, long long received, long long total);
    // Both mark the item Finished (or Failed) and notify. reportFail takes
    // ownership of running on the worker's thread.
    void reportFinish(unsigned id, long long finalTotal);
    void reportFail(unsigned id, std::wstring reason);
    // Returns the destination path for an item the worker is transferring,
    // or empty if the item disappeared (cancelled and removed).
    std::wstring destinationFor(unsigned id) const;
    // Used by the WinHTTP worker between chunks to detect a cancel or
    // removal. Cheap; takes the m_lock briefly.
    bool stillActive(unsigned id) const;

    // Window the browser pumps Download notifications through. Set once at
    // startup; nullptr falls back to a direct call (used by the
    // Downloads singleton itself in tests).
    void setOwnerWindow(HWND window) { m_ownerWindow = window; }

    // Snapshot of the list, newest first. Deliberately by value: the WebKit
    // callbacks insert into m_items from network IPC threads, so handing out a
    // reference would let a painter read a std::vector that is being
    // reallocated underneath it.
    std::vector<DownloadItem> items() const;
    bool hasActive() const;
    // Combined progress (0..1) of the active downloads whose size is known.
    double progress() const;

    void cancel(unsigned id);
    void remove(unsigned id);
    void clearFinished();

    // Reads the true byte count off disk for every active download that is
    // still going through WKDownload. didWriteData does fire on this build
    // (lumen.log from build 15 logged it 3,599 times during one stalled
    // transfer), but always with totalBytesWritten == 0, which means the
    // network process is not actually streaming. The file on disk is the
    // truth: polling it gives the panel a real number and lets us mark the
    // download Finished the instant the bytes match the response size.
    // Transfers that bypass WKDownload (the WinHTTP path in
    // download_transfer.cpp) update item.received directly, so this polling
    // becomes a safety net for them as well.
    void refreshFromDisk();

    // Called on the UI thread when anything changes. `started` is true when a
    // new download has just begun.
    std::function<void(bool started)> onChanged;

private:
    // Guards m_items and m_clients. The download callbacks arrive on WebKit's
    // network IPC threads while the panel, the Downloads page and the cancel
    // buttons all run on the UI thread, so both sides need this.
    mutable CRITICAL_SECTION m_lock;

    // Both require m_lock to be held by the caller.
    DownloadItem* findLocked(WKDownloadRef download);
    DownloadItem* findIdLocked(unsigned id);

    void notify(bool started);

    static WKStringRef decideDestination(WKDownloadRef download, WKURLResponseRef response, WKStringRef suggestedFilename, const void* clientInfo);
    static void didWriteData(WKDownloadRef download, long long bytesWritten, long long totalBytesWritten, long long totalBytesExpectedToWrite, const void* clientInfo);
    static void didFinish(WKDownloadRef download, const void* clientInfo);
    static void didFail(WKDownloadRef download, WKErrorRef error, WKDataRef resumeData, const void* clientInfo);

    std::vector<DownloadItem> m_items;
    unsigned m_nextId { 1 };
    HWND m_ownerWindow { nullptr };

    // WKDownloadSetClient stores a raw pointer to the callback table and
    // dereferences it for every callback that follows, so the table has to
    // outlive attach(). One table per live download, released when the
    // download reaches a terminal state. The callbacks only run for items
    // that go through WebKit's own download machinery; the WinHTTP path in
    // download_transfer.cpp updates item.received directly and bypasses all
    // of this for the URL-shape rules in webkit_host.cpp.
    std::map<WKDownloadRef, std::unique_ptr<WKDownloadClientV0>> m_clients;
};

// "3.4 MB", "812 KB" ...
std::wstring formatBytes(long long bytes);

} // namespace lumen