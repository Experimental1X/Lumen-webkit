# Lumen

A WebKit-based browser for Windows, designed to follow the look and feel of
Safari 26. Personal project; not for distribution.

Lumen is built in two stages so that interface work does not require
rebuilding WebKit:

1. **`webkit-sdk` workflow** builds WebKit once and publishes a *WebKit SDK*
   (headers, import libraries, DLLs, helper processes) as a GitHub Release.
2. **`build-lumen` workflow** downloads that SDK and compiles the app against
   it. This takes minutes.

## Getting started

1. Push this repository to GitHub.
2. In the repo, open **Settings, Actions, General, Workflow permissions** and
   choose **Read and write permissions** (the SDK workflow publishes a release).
3. Open the **Actions** tab, choose **webkit-sdk**, and run it. A WebKit build is
   long. If the run ends with "WebKit build incomplete", run it again; progress
   is cached and the build continues.
4. When the SDK release exists, run **build-lumen**.
5. Download the `lumen-windows-x64` artifact, unzip the whole thing into one
   folder, and run `Lumen.exe`. Runtime problems are written to `lumen.log`
   next to the exe.

## Layout

| Path | Purpose |
| --- | --- |
| `.github/workflows/webkit-sdk.yml` | Builds WebKit and publishes the SDK release |
| `.github/workflows/build-lumen.yml` | Builds the app against the SDK |
| `scripts/build-webkit.ps1` | WebKit build environment and invocation |
| `scripts/package-sdk.ps1` | Finds headers, libs and runtime files and zips the SDK |
| `src/main.cpp` | Window and message loop |
| `src/webkit_host.*` | WebKit context and web view wrapper |
| `src/util.*` | String conversion and `lumen.log` |
| `docs/ROADMAP.md` | Planned milestones |

## Status

Milestone 1 (this commit): app links against the SDK and renders a page in a
plain window. No browser chrome yet.

## Known risks in milestone 1

These are the places most likely to need adjusting after the first runs:

- **Disk space and time on the hosted runner.** The WebKit build is large. The
  workflow prints free disk space at the start. If the runner runs out of space,
  or the cache cannot hold the build directory, a larger runner or a local build
  of the SDK is the fallback.
- **WebKit build layout.** `package-sdk.ps1` searches for headers and libraries
  instead of assuming paths and prints a listing if something required is
  missing. Paste that listing to get it fixed.
- **Build tools.** The tool list is carried over from an earlier workflow and
  may need tuning.
