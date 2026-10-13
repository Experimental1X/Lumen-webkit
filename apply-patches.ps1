# Apply the Lumen patches from `Update 5 (patched)\src\` into this repo.
# Run this from the root of your git checkout (where `src/` lives).

$ErrorActionPreference = "Stop"

$source = "C:\Users\KK\Downloads\Compressed\Lumen\Update 5 (patched)\src"
$dest   = Join-Path (Get-Location) "src"

if (-not (Test-Path $source)) {
    Write-Error "Source folder not found: $source"
    exit 1
}
if (-not (Test-Path $dest)) {
    Write-Error "Destination src folder not found: $dest"
    exit 1
}

Get-ChildItem -Path $source -File | ForEach-Object {
    $target = Join-Path $dest $_.Name
    Copy-Item -Path $_.FullName -Destination $target -Force
    Write-Host "updated: src\$($_.Name)"
}

git add src/

# Show what changed before committing
git status --short

# Tell the user what to do next
Write-Host ""
Write-Host "=== Patch files dropped into src/. Review 'git diff --staged', then: ==="
Write-Host "    git commit -m 'Fix crashes, downloads, phantom YT, Show All page'"
Write-Host "    git push"
Write-Host ""
Write-Host "That triggers Azure Pipelines to rebuild at D:\a\Lumen-webkit with the fixes in place."
