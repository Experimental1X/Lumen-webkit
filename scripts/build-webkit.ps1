# Builds WebKit (Release) with the Windows toolchain WebKit expects.
# Called by the webkit-sdk workflow. Environment setup is carried over from
# the earlier Aurora workflow, which is the best evidence of what the build
# needs on a GitHub-hosted runner.
param(
    [Parameter(Mandatory = $true)][string]$WebKitDir
)

$ErrorActionPreference = "Stop"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found. Visual Studio with the C++ workload is required." }

$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw "Visual Studio with C++ tools not found." }

$vcvars = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$vcpkgRoot = Join-Path $vsInstall "VC\vcpkg"
if (-not (Test-Path (Join-Path $vcpkgRoot "vcpkg.exe"))) { throw "Visual Studio vcpkg.exe not found under $vcpkgRoot" }

$env:VCPKG_ROOT = $vcpkgRoot
$env:PATH = "C:\xampp\apache\bin;C:\xampp\perl\bin;C:\xampp\mysql\bin;C:\Program Files\CMake\bin;C:\Program Files\Ninja;C:\Program Files\Git\usr\bin;C:\Program Files\Git\bin;C:\Program Files\LLVM\bin;C:\Program Files (x86)\GnuWin32\bin;$env:PATH"
$env:PYTHON = "python"
$env:PYTHON3 = "python"
$env:CMAKE_EXPORT_COMPILE_COMMANDS = "OFF"
$env:WEBKIT_LIBRARIES = Join-Path $WebKitDir "WebKitLibraries\win"
$env:WEBKIT_OUTPUTDIR = Join-Path $WebKitDir "WebKitBuild"
$env:WEBKIT_TESTFONTS = Join-Path $WebKitDir "Tools\WebKitTestRunner\fonts"
$env:DUMPRENDERTREE_TEMP = $env:TEMP
$env:CC = "clang-cl"
$env:CXX = "clang-cl"
$env:PATH = "$env:WEBKIT_LIBRARIES\bin;$env:PATH"

Write-Host "Building WebKit in $WebKitDir"
cmd /c "call `"$vcvars`" && cd /d `"$WebKitDir`" && perl Tools\Scripts\build-webkit --release --cmakeargs=`"-DCMAKE_EXPORT_COMPILE_COMMANDS=OFF`""
if ($LASTEXITCODE -ne 0) { throw "build-webkit failed with exit code $LASTEXITCODE" }
