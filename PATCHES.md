# Lumen — patches from this session

This folder is a copy of `Update 5/` with four fixes applied, intended to be
a drop-in replacement for whatever copy of the source you build from.
Run your existing `cmake -B build -DLUMEN_SDK_DIR=...` against this folder
without any other change.

## Fixes in this build

### 1. Browser crash after ~5 minutes (`webkit_host.cpp`)
`WebView::~WebView` now always `m_view.leak()` so the `Ref<WKViewRef>`
destructor never `WKRelease`s a WKView while WebKit is still wrapping up
`WKPageClose`. From the `lumen.log` you shared, the deterministic
`WebKit2.dll +0x6C7606` crash was a use-after-free in WebKit's mid-teardown
release path. Memory cost: one leaked `WKView` per closed tab, freed at
process exit.

### 2. Phantom downloads on YouTube.com (`webkit_host.cpp`)
`decideNavigationResponse` no longer converts every unknown-MIME response
into a download. It only does so when the response carries
`Content-Disposition: attachment`. Subresource fetches inside the main
frame now go to the page itself. `<a download>` links still become
downloads via `decideNavigationAction`.

### 3. Downloads stuck on "0 bytes of N" / "Show All in Page" blank tab
Two coupled changes:

* `downloads_page.h` — removed `<meta http-equiv="refresh" content="1.5">`
  so the page no longer self-reloads.
* `downloads.h` / `downloads.cpp` / `browser.cpp` / `main.cpp` —
  introduced `WM_LUMEN_DOWNLOAD_CHANGED`. `Downloads::notify` posts that
  message instead of calling `onChanged` directly, and the main window
  proc handles it by calling `Browser::onDownloadsChanged`. This way every
  browser-side UI update runs on the UI thread. Most likely the original
  bug was that WebKit was calling `didWriteData` from a non-UI thread and
  our `notify` was either silently dropped or racing with paint.
* `browser.h` — added `m_downloadsPageTab` (the index of the full
  downloads page tab, or `size_t(-1)`); `Browser::onDownloadsChanged`
  re-renders that tab via `view->loadHtml(...)` whenever state changes,
  so removing the meta refresh is invisible to the user.

### 4. YouTube 360p
Not fixed in this build. Windows WebKit2 lacks H.264 / AAC / Widevine, so
the Safari User-Agent spoof can't unlock HD; codec negotiation happens
against installed decoders, not the UA. Three opt-in follow-ups are
described in the chat — pick one and I'll do it next.

## How to apply

### Option A — replace your working copy
If your `Update 5/src/` is the copy you build from:

```
cp -r "Update 5 (patched)/src/"* "Update 5/src/"
cmake --build build --config Release
```

### Option B — push to your own git repo
From the directory of your git checkout:

```
cp "Update 5 (patched)/src/downloads.cpp"      src/
cp "Update 5 (patched)/src/downloads.h"        src/
cp "Update 5 (patched)/src/downloads_page.h"   src/
cp "Update 5 (patched)/src/browser.cpp"        src/
cp "Update 5 (patched)/src/browser.h"          src/
cp "Update 5 (patched)/src/main.cpp"           src/
cp "Update 5 (patched)/src/webkit_host.cpp"    src/
git add src/
git commit -m "Fix downloads and 5-min WebKit2 crash"
```

## Files changed (vs Update 4 baseline, just so you can `git diff`)

* `src/browser.cpp`
* `src/browser.h`
* `src/main.cpp`
* `src/webkit_host.cpp`

Files added in newer updates that weren't in Update 4, also patched in this
build:

* `src/downloads.cpp`
* `src/downloads.h`
* `src/downloads_page.h`

`src/webkit_host.h` was unchanged in this session.

## GitHub note
There's no GitHub connector installed in this Claude Desktop session
(the connector / plugin libraries returned empty for `github` / `git` /
`repository` searches). When you install one — or if you already have
one elsewhere in your workflow — point me at it and I can push
straight through.
