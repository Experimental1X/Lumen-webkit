# Packages a finished WebKit build into a self-contained "WebKit SDK" zip:
#
#   include/WebKit/*.h            public C API headers
#   include/JavaScriptCore/*.h    JavaScriptCore C API headers
#   lib/*.lib                     import libraries (WebKit2.lib is required)
#   runtime/                      DLLs, helper processes and resources
#   MANIFEST.txt, VERSION.txt     what was found, and where
#
# The WebKit build layout is not guaranteed, so this script searches for
# files instead of assuming paths, writes down what it found, and fails with
# a readable listing when a required piece is missing.
param(
    [Parameter(Mandatory = $true)][string]$WebKitDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$WebKitSha = "unknown"
)

$ErrorActionPreference = "Stop"

$release = Join-Path $WebKitDir "WebKitBuild\Release"
if (-not (Test-Path -LiteralPath $release -PathType Container)) {
    throw "WebKit build output not found at $release"
}

$stage = Join-Path $OutDir "stage"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }

$incWK = Join-Path $stage "include\WebKit"
$incJSC = Join-Path $stage "include\JavaScriptCore"
$libOut = Join-Path $stage "lib"
$rtOut = Join-Path $stage "runtime"
New-Item -ItemType Directory -Force -Path $incWK, $incJSC, $libOut, $rtOut | Out-Null

$manifest = New-Object System.Collections.Generic.List[string]
function Note([string]$message) {
    Write-Host $message
    $manifest.Add($message)
}

function Copy-Headers([string]$from, [string]$to) {
    if (-not (Test-Path -LiteralPath $from -PathType Container)) { return 0 }
    $files = @(Get-ChildItem -LiteralPath $from -Filter "*.h" -File)
    foreach ($file in $files) { Copy-Item -LiteralPath $file.FullName -Destination $to -Force }
    return $files.Count
}

function Find-First([string]$root, [string]$name, [string]$preferPattern = "") {
    $matches = @(Get-ChildItem -LiteralPath $root -Recurse -Filter $name -File -ErrorAction SilentlyContinue)
    if ($matches.Count -eq 0) { return $null }
    if ($preferPattern) {
        $preferred = @($matches | Where-Object { $_.DirectoryName -match $preferPattern })
        if ($preferred.Count -gt 0) { return $preferred[0] }
    }
    return $matches[0]
}

# --- Headers: source tree first, then anything the build generated -----------
Note "== headers =="
foreach ($dir in @(
        "Source\WebKit\Shared\API\c",
        "Source\WebKit\Shared\API\c\win",
        "Source\WebKit\UIProcess\API\C",
        "Source\WebKit\UIProcess\API\C\win")) {
    $count = Copy-Headers (Join-Path $WebKitDir $dir) $incWK
    Note "source $dir -> $count headers"
}
$count = Copy-Headers (Join-Path $WebKitDir "Source\JavaScriptCore\API") $incJSC
Note "source Source\JavaScriptCore\API -> $count headers"

$builtWK = Find-First $release "WKContext.h"
if ($builtWK) {
    $count = Copy-Headers $builtWK.DirectoryName $incWK
    Note "build tree $($builtWK.DirectoryName) -> $count headers"
} else {
    Note "build tree: no WKContext.h found (using source headers only)"
}
$builtJSC = Find-First $release "JavaScript.h"
if ($builtJSC) {
    $count = Copy-Headers $builtJSC.DirectoryName $incJSC
    Note "build tree $($builtJSC.DirectoryName) -> $count headers"
}

# --- Import libraries ---------------------------------------------------------
Note "== libraries =="
foreach ($name in @("WebKit2.lib", "JavaScriptCore.lib", "WebCore.lib", "WTF.lib", "PAL.lib")) {
    $found = Find-First $release $name '\\lib(64)?$'
    if ($found) {
        Copy-Item -LiteralPath $found.FullName -Destination $libOut -Force
        Note "$name <- $($found.FullName)"
    } else {
        Note "$name MISSING"
    }
}

# --- Runtime files ------------------------------------------------------------
Note "== runtime =="
$binDirs = @("bin64", "bin") | ForEach-Object { Join-Path $release $_ } |
    Where-Object { Test-Path -LiteralPath $_ -PathType Container }
if ($binDirs.Count -eq 0) { throw "Neither bin64 nor bin exists under $release" }

$excluded = '^(MiniBrowser|WebKitTestRunner|DumpRenderTree|TestWebKitAPI|ImageDiff|TestRunner)'
foreach ($dir in $binDirs) {
    Get-ChildItem -LiteralPath $dir -File |
        Where-Object { $_.Extension -in ".dll", ".exe", ".dat", ".pak" -and $_.Name -notmatch $excluded } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $rtOut -Force }

    Get-ChildItem -LiteralPath $dir -Directory -Filter "*.resources" |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $rtOut $_.Name) -Recurse -Force }
}

$thirdParty = Join-Path $WebKitDir "WebKitLibraries\win\bin"
if (Test-Path -LiteralPath $thirdParty -PathType Container) {
    Get-ChildItem -LiteralPath $thirdParty -Filter "*.dll" -File |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $rtOut -Force }
}
Note "runtime files: $(@(Get-ChildItem -LiteralPath $rtOut -Recurse -File).Count)"

# --- Verify -------------------------------------------------------------------
$required = @(
    @{ Path = Join-Path $libOut "WebKit2.lib"; Why = "import library the app links against" },
    @{ Path = Join-Path $incWK "WKContext.h"; Why = "WebKit C API header" },
    @{ Path = Join-Path $incWK "WKView.h"; Why = "Windows WKView header" },
    @{ Path = Join-Path $incWK "WKPageStateClient.h"; Why = "page state callbacks header" },
    @{ Path = Join-Path $rtOut "WebKit2.dll"; Why = "WebKit runtime" },
    @{ Path = Join-Path $rtOut "JavaScriptCore.dll"; Why = "JavaScriptCore runtime" },
    @{ Path = Join-Path $rtOut "WebCore.dll"; Why = "WebCore runtime" },
    @{ Path = Join-Path $rtOut "WebKitWebProcess.exe"; Why = "web content process" },
    @{ Path = Join-Path $rtOut "WebKitNetworkProcess.exe"; Why = "network process" }
)

$problems = @()
foreach ($item in $required) {
    if (-not (Test-Path -LiteralPath $item.Path)) { $problems += "missing: $($item.Path)  ($($item.Why))" }
}

if ($problems.Count -gt 0) {
    Write-Host "`n!! SDK is incomplete:"
    $problems | ForEach-Object { Write-Host "   $_" }
    Write-Host "`n--- *.lib files in the build tree ---"
    Get-ChildItem -LiteralPath $release -Recurse -Filter "*.lib" -File -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host "   $($_.FullName)" }
    Write-Host "`n--- top-level build directories ---"
    Get-ChildItem -LiteralPath $release -Directory | ForEach-Object { Write-Host "   $($_.FullName)" }
    foreach ($dir in $binDirs) {
        Write-Host "`n--- $dir ---"
        Get-ChildItem -LiteralPath $dir | Select-Object -First 80 | ForEach-Object { Write-Host "   $($_.Name)" }
    }
    throw "WebKit SDK packaging failed: $($problems.Count) required item(s) missing."
}

$manifest | Set-Content -LiteralPath (Join-Path $stage "MANIFEST.txt")
@("WebKit SHA: $WebKitSha", "Packaged (UTC): $((Get-Date).ToUniversalTime().ToString('o'))") |
    Set-Content -LiteralPath (Join-Path $stage "VERSION.txt")

$zip = Join-Path $OutDir "webkit-sdk-win-x64.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -Force
Write-Host ("SDK written: {0} ({1:N0} MB)" -f $zip, ((Get-Item -LiteralPath $zip).Length / 1MB))
