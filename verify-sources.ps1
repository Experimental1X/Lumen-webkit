# Runs BEFORE the build. Fails fast, by filename, when the working tree is not
# exactly the tree the shipped zip contained.
#
# This exists because of a failure that was genuinely baffling. On 2026-10-22
# the brand-new gfx.h compiled, browser.cpp compiled, and two files nobody had
# touched in four updates - downloads.cpp and download_transfer.cpp - threw the
# C2275/C2660 errors that had been fixed back in update-17. The compiler was
# right and the zip was right; the checkout CI was building simply held older
# revisions of those two files. Nothing in a compile log says "this file is
# stale", and it reads exactly like I had written broken code.
#
# Add to the workflow above the `cmake --build` step:
#
#   - name: Verify sources match the shipped zip
#     shell: pwsh
#     run: ./verify-sources.ps1
#
# SOURCES.sha256 is regenerated whenever a new zip is published, so it always
# describes the tree that zip contains. If this step fails, delete src/,
# re-extract the whole zip over the repo root, and commit every file - do not
# patch files one at a time.

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }
$manifest = Join-Path $root 'SOURCES.sha256'

if (-not (Test-Path $manifest)) {
    throw "verify-sources.ps1: SOURCES.sha256 is missing next to this script."
}

# Hash the file's TEXT with line endings normalised to LF - not its raw bytes.
#
# This is not fussiness, it is the whole ballgame. GitHub Desktop on Windows
# checks out with core.autocrlf=true, so a fresh clone puts CRLF on disk while
# the repository stores LF. Get-FileHash would therefore hash a CRLF file and
# compare it against an LF manifest, and report every single file STALE - on the
# very first run, on a tree that is completely correct. A guard that always
# fires is worse than no guard, because the obvious response to it is to delete
# it, and then the real staleness goes undetected too.
#
# Normalising both sides to LF makes the answer independent of how the checkout
# happened to write the file. A real edit still changes the hash; only the
# line-ending convention stops mattering.
function Get-NormalizedSha256([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    # Strip a BOM if present, so an editor that adds one is not a diff either.
    if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
    $text = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($text))
        return ([System.BitConverter]::ToString($hash) -replace '-', '').ToLowerInvariant()
    } finally { $sha.Dispose() }
}

$bad = New-Object System.Collections.Generic.List[string]
$checked = 0

foreach ($raw in [System.IO.File]::ReadAllLines($manifest)) {
    $line = $raw.Trim()
    if ($line.Length -eq 0) { continue }
    if ($line.StartsWith('#')) { continue }

    # Standard sha256sum line: 64 hex chars, whitespace, optional '*' (binary
    # mode), then the path. Paths use forward slashes; convert for Windows.
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+\*?(.+)$') {
        throw "verify-sources.ps1: cannot parse this line of SOURCES.sha256: $line"
    }

    $expected = $Matches[1].ToLowerInvariant()
    $rel = $Matches[2] -replace '/', '\'
    $path = Join-Path $root $rel
    $checked++

    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $bad.Add("MISSING  $rel")
        continue
    }

    if ((Get-NormalizedSha256 $path) -ne $expected) {
        $bad.Add("STALE    $rel")
    }
}

if ($bad.Count -gt 0) {
    Write-Host ''
    Write-Host 'The working tree does NOT match the shipped sources:' -ForegroundColor Red
    foreach ($line in $bad) { Write-Host "  $line" -ForegroundColor Red }
    Write-Host ''
    Write-Host 'Fix: delete the src/ folder, re-extract the whole zip over the repo' -ForegroundColor Yellow
    Write-Host 'root, then commit every file. Do not patch files one at a time.' -ForegroundColor Yellow
    exit 1
}

Write-Host "Sources match SOURCES.sha256 ($checked files)."