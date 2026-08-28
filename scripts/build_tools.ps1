[CmdletBinding()]
param(
    [Alias("p")]
    [ValidateSet("windows", "macos", "linux", "android")]
    [string]$Platform,
    [Alias("a")]
    [string]$Architecture,
    [Alias("t")]
    [string]$Toolchain,
    [Alias("n")]
    [string]$AndroidNdk,
    [Alias("b")]
    [string]$BuildDirectory,
    [Alias("o")]
    [string]$OutputDirectory,
    [Alias("h")]
    [switch]$Help
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Show-Usage {
    Write-Output @"
Usage: build_tools.ps1 [-p|-Platform <platform>] [-a|-Architecture <architecture>]
                       [-t|-Toolchain <path>] [-n|-AndroidNdk <path>]
                       [-b|-BuildDirectory <path>] [-o|-OutputDirectory <path>]

Builds and installs the HuxerUI host tools for Windows, macOS, Linux, or Android.
"@
}

if ($Help) {
    Show-Usage
    exit 0
}

function Require-Command([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw "HuxerUI host tools require '$Name' on PATH"
    }
    return $command.Source
}

function Get-AbsolutePath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $Path))
}

$sourceDirectory = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$nativePlatform = if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [System.Runtime.InteropServices.OSPlatform]::Windows)) {
    "windows"
} elseif ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [System.Runtime.InteropServices.OSPlatform]::OSX)) {
    "macos"
} elseif ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [System.Runtime.InteropServices.OSPlatform]::Linux)) {
    $termuxVersion = [Environment]::GetEnvironmentVariable("TERMUX_VERSION")
    $androidRoot = [Environment]::GetEnvironmentVariable("ANDROID_ROOT")
    if ($termuxVersion -or ($androidRoot -and (Test-Path -LiteralPath "/system/bin/getprop" -PathType Leaf))) {
        "android"
    } else {
        "linux"
    }
} else {
    throw "HuxerUI host tools cannot identify the current platform"
}
if (-not $Platform) {
    $Platform = $nativePlatform
}

$nativeArchitecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
if ($nativeArchitecture -in @("amd64", "x64")) {
    $nativeArchitecture = "x86_64"
} elseif ($nativeArchitecture -in @("aarch64", "arm64")) {
    $nativeArchitecture = switch ($nativePlatform) {
        "linux" { "aarch64" }
        "android" { "arm64-v8a" }
        default { "arm64" }
    }
}
if (-not $Architecture) {
    $Architecture = switch ($Platform) {
        "android" { "arm64-v8a" }
        "windows" { "x86_64" }
        "linux" { if ($nativeArchitecture -eq "x86_64") { "x86_64" } else { "aarch64" } }
        "macos" { if ($nativeArchitecture -eq "x86_64") { "x86_64" } else { "arm64" } }
    }
} else {
    if ($Architecture -in @("amd64", "x64")) {
        $Architecture = "x86_64"
    } elseif ($Architecture -in @("aarch64", "arm64")) {
        $Architecture = switch ($Platform) {
            "linux" { "aarch64" }
            "android" { "arm64-v8a" }
            default { "arm64" }
        }
    }
}

$supportedPackage = "$Platform/$Architecture"
if ($supportedPackage -notin @(
        "windows/x86_64",
        "macos/arm64",
        "macos/x86_64",
        "linux/aarch64",
        "linux/x86_64",
        "android/arm64-v8a"
    )) {
    throw "HuxerUI host tools do not support $supportedPackage"
}

if ($Platform -ne $nativePlatform -and $Platform -ne "android" -and -not $Toolchain) {
    throw "HuxerUI host tools require -Toolchain to build $Platform tools on $nativePlatform"
}
if ($Platform -eq "linux" -and $Architecture -ne $nativeArchitecture -and -not $Toolchain) {
    throw "HuxerUI host tools require -Toolchain to build Linux $Architecture tools on $nativeArchitecture"
}

if ($Platform -eq "android" -and -not $Toolchain) {
    if (-not $AndroidNdk) {
        $AndroidNdk = if ($env:ANDROID_NDK_HOME) { $env:ANDROID_NDK_HOME } else { $env:ANDROID_NDK_ROOT }
    }
    if (-not $AndroidNdk -and $env:ANDROID_HOME) {
        $gradleProperties = Get-Content -LiteralPath (Join-Path $sourceDirectory "platform/android/gradle.properties")
        $ndkProperty = $gradleProperties | Where-Object { $_ -match '^huxeruiNdkVersion=' } | Select-Object -First 1
        if (-not $ndkProperty) {
            throw "HuxerUI host tools cannot resolve the Android NDK version"
        }
        $ndkVersion = $ndkProperty.Substring($ndkProperty.IndexOf('=') + 1)
        $AndroidNdk = Join-Path $env:ANDROID_HOME "ndk/$ndkVersion"
    }
    if (-not $AndroidNdk) {
        throw "Set an Android NDK environment variable or pass -AndroidNdk"
    }
    $Toolchain = Join-Path $AndroidNdk "build/cmake/android.toolchain.cmake"
}

if ($Toolchain) {
    $Toolchain = Get-AbsolutePath $Toolchain
    if (-not (Test-Path -LiteralPath $Toolchain -PathType Leaf)) {
        throw "HuxerUI host tools toolchain file is missing: $Toolchain"
    }
}

$cmake = Require-Command "cmake"
$null = Require-Command "ninja"
$BuildDirectory = if ($BuildDirectory) {
    Get-AbsolutePath $BuildDirectory
} else {
    Join-Path $sourceDirectory "build/tools/$Platform/$Architecture"
}
$OutputDirectory = if ($OutputDirectory) {
    Get-AbsolutePath $OutputDirectory
} else {
    Join-Path $sourceDirectory "tools/prebuilt/$Platform/$Architecture"
}
New-Item -ItemType Directory -Path $BuildDirectory, $OutputDirectory -Force | Out-Null

function Build-Tool(
    [string]$SourcePath,
    [string]$ToolBuildDirectory,
    [string]$TargetName
) {
    $configureArguments = @(
        "-S", $SourcePath,
        "-B", $ToolBuildDirectory,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release"
    )
    if ($Toolchain) {
        $configureArguments += "-DCMAKE_TOOLCHAIN_FILE=$Toolchain"
    }
    if ($Platform -eq "macos") {
        $configureArguments += "-DCMAKE_OSX_ARCHITECTURES=$Architecture"
    } elseif ($Platform -eq "android") {
        $configureArguments += @("-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-24")
    }

    & $cmake @configureArguments
    if ($LASTEXITCODE -ne 0) {
        throw "HuxerUI host tools configuration failed"
    }
    & $cmake --build $ToolBuildDirectory --target $TargetName
    if ($LASTEXITCODE -ne 0) {
        throw "HuxerUI host tool build failed: $TargetName"
    }
    & $cmake --install $ToolBuildDirectory --strip --prefix $OutputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "HuxerUI host tool installation failed: $TargetName"
    }
}

Build-Tool `
    (Join-Path $sourceDirectory "tools/codegen") `
    (Join-Path $BuildDirectory "hcg") `
    "huxerui_codegen"
Build-Tool `
    (Join-Path $sourceDirectory "tools/resource_compiler") `
    (Join-Path $BuildDirectory "hrc") `
    "huxerui_resource_compiler"

Write-Output "HuxerUI $Platform $Architecture host tools written to $OutputDirectory"
