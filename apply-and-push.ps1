# apply-and-push.ps1 — drop the Lumen patches into your git repo, commit, push.
# Run this from the root of your Lumen-webkit checkout. Requires git on PATH.

$ErrorActionPreference = "Stop"

$source = "C:\Users\KK\Downloads\Compressed\Lumen\Update 5 (patched)\src"
$here   = Get-Location

# Sanity checks
if (-not (Test-Path "$here\src")) {
    Write-Error "No 'src' folder in the current directory ($here).`nThis script must run from the root of your Lumen-webkit git checkout."
    exit 1
}
if (-not (Test-Path ".git")) {
    Write-Error "Not a git repository ($here).`nRun this from the root of your Lumen-webkit checkout, the folder that contains .git/"
    exit 1
}
if (-not (Test-Path $source)) {
    Write-Error "Patch folder not found at $source"
    exit 1
}

# Configure git user if unset in this clone (CI pushes need an identity)
if (-not (git config user.email)) { git config user.email "claude@local" }
if (-not (git config user.name))  { git config user.name  "Claude" }

# Confirm we're going to push to the right place, then list it once
$remote = git remote get-url origin 2>$null
if (-not $remote) {
    Write-Error "This clone has no 'origin' remote. Add one with:`n  git remote add origin https://github.com/<you>/Lumen-webkit.git"
    exit 1
}
Write-Host "Remote: $remote"

# Drop every file from the patched folder into src\
Get-ChildItem -Path $source -File | ForEach-Object {
    $target = Join-Path "$here\src" $_.Name
    Copy-Item -Path $_.FullName -Destination $target -Force
    Write-Host "  updated: src\$($_.Name)"
}

# Stage and review
git add src/
Write-Host ""
Write-Host "Diff to be committed:"
Write-Host "======================="
git diff --cached --stat
Write-Host ""

# Commit
git commit -m "Fix 5-min crash, phantom YouTube downloads, Show All blank, downloads stuck at 0/N"

# Push
Write-Host ""
Write-Host "Pushing to $remote ..."
git push

Write-Host ""
Write-Host "Done. GitHub Actions should pick up the commit and rebuild. Watch the Actions tab for status."
