#pragma once

#include <string>

namespace lumen {

// Why this module exists.
//
// On Apple's WinCairo port, the WKDownload object hands us correct metadata
// (decideDestinationWithResponse fires, the response's Content-Length is
// reported in the panel as a real total), but the network process never
// streams the body. didWriteData fires every ~65 ms with totalBytesWritten
// == 0 indefinitely. The user's build-15 lumen.log captured this exactly:
// 3,599 callbacks during one stalled transfer, the destination file still
// 0 bytes, never any "Download finished" or "Download complete". The
// engine's data path is dead for this class of URL.
//
// For the URLs the download URL rule matches in
// WebView::decideNavigationAction - FileCR mirrors, GitHub Releases, the
// various binary-extension link shapes - we cancel the WebKit navigation
// and fetch them ourselves through WinHTTP on a worker thread. The
// Downloads panel and the downloads page both read items() regardless of
// which path wrote the bytes, so the UI is unchanged.

// Detaches a worker that runs the WinHTTP transfer for an existing item.
// The item has already been pushed onto the Downloads list with the right
// destination path; we pass that id back here and let the worker stream
// the body to disk through Downloads::reportProgress.
//
// resumeFrom is the number of bytes already sitting at the destination.
// 0 starts a fresh transfer and truncates the file; anything higher sends
// `Range: bytes=<resumeFrom>-` and appends, which is what makes a paused
// download pick up where it stopped.
//
// generation must match the item's current generation. The worker retires
// itself as soon as Downloads reports a different one, which is what keeps a
// pause-then-resume from leaving two threads writing the same file.
void startWinhttpTransfer(unsigned id, std::wstring url, long long resumeFrom,
    unsigned generation);

// Implementation detail. The detached thread runs this; nothing else does.
void runWinhttpTransfer(unsigned id, std::wstring url, long long resumeFrom,
    unsigned generation);

} // namespace lumen
