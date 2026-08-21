[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [ValidateRange(1, 1024)]
    [int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount),
    [switch]$Help
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Show-Usage {
    Write-Output @"
Usage: package_sdk.ps1 [-BuildDirectory <path>] [-OutputDirectory <path>]
                       [-Configuration Debug|Release] [-Jobs <count>]

Builds Android and Web target artifacts, then packages a complete SDK for the
current host. Android SDK/NDK, Java, and Emscripten 4.0.19 must be installed.
"@
}

if ($Help) {
    Show-Usage
    exit 0
}

function Get-AbsolutePath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $Path))
}

function Require-Command([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw "HuxerUI SDK packaging requires '$Name' on PATH"
    }
    return $command.Source
}

function Initialize-WindowsToolchain {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        return
    }
    if (Get-Command cl -ErrorAction SilentlyContinue) {
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "HuxerUI SDK packaging requires Visual Studio with C++ tools"
    }
    $installationPath = (& $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath | Select-Object -First 1)
    if (-not $installationPath) {
        throw "HuxerUI SDK packaging cannot find Visual Studio C++ tools"
    }
    $developerCommand = Join-Path $installationPath "Common7/Tools/VsDevCmd.bat"
    if (-not (Test-Path -LiteralPath $developerCommand -PathType Leaf)) {
        throw "Visual Studio developer environment is missing: $developerCommand"
    }

    $environmentCommand = "call `"$developerCommand`" -arch=x64 -host_arch=x64 >nul && set"
    $environmentLines = & $env:ComSpec /d /c $environmentCommand
    if ($LASTEXITCODE -ne 0) {
        throw "Cannot initialize the Visual Studio developer environment"
    }
    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf('=')
        if ($separator -le 0) {
            continue
        }
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        [Environment]::SetEnvironmentVariable($name, $value, "Process")
    }
    $null = Require-Command "cl"
}

function Invoke-Checked(
    [string]$Executable,
    [string[]]$Arguments,
    [string]$WorkingDirectory
) {
    Write-Host "> $Executable $($Arguments -join ' ')"
    Push-Location $WorkingDirectory
    try {
        & $Executable @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed with exit code ${LASTEXITCODE}: $Executable"
        }
    } finally {
        Pop-Location
    }
}

function Reset-OwnedDirectory([string]$Path, [string]$Owner) {
    $absolutePath = [System.IO.Path]::GetFullPath($Path)
    $absoluteOwner = [System.IO.Path]::GetFullPath($Owner).TrimEnd('\', '/')
    $ownedPrefix = $absoluteOwner + [System.IO.Path]::DirectorySeparatorChar
    if (-not $absolutePath.StartsWith($ownedPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace a directory outside the SDK build root: $absolutePath"
    }
    if (Test-Path -LiteralPath $absolutePath) {
        Remove-Item -LiteralPath $absolutePath -Recurse -Force
    }
    New-Item -ItemType Directory -Path $absolutePath -Force | Out-Null
}

$sourceDirectory = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildDirectory = if ($BuildDirectory) {
    Get-AbsolutePath $BuildDirectory
} else {
    Join-Path $sourceDirectory "build/sdk"
}
$outputDirectory = if ($OutputDirectory) {
    Get-AbsolutePath $OutputDirectory
} else {
    Join-Path $sourceDirectory "release-assets"
}
$platformArtifactRoot = Join-Path $buildDirectory "platform-artifacts"
$androidExtractDirectory = Join-Path $buildDirectory "android-aar"
$webBuildDirectory = Join-Path $buildDirectory "web"
$hostBuildDirectory = Join-Path $buildDirectory "host"

New-Item -ItemType Directory -Path $buildDirectory, $outputDirectory -Force | Out-Null

$cmake = Require-Command "cmake"
$cpack = Require-Command "cpack"
$null = Require-Command "java"
$jar = Require-Command "jar"
$emcmake = Require-Command "emcmake"
$emcc = Require-Command "emcc"
Initialize-WindowsToolchain
if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
    $null = Require-Command "ninja"
}

$sdkDefinition = Get-Content -LiteralPath (Join-Path $sourceDirectory "cmake/HuxerUISdk.cmake") -Raw
if ($sdkDefinition -notmatch 'set\(HUXERUI_WEB_EMSCRIPTEN_VERSION "([^"]+)"\)') {
    throw "Cannot resolve the HuxerUI Web Emscripten version"
}
$webVersion = $Matches[1]
$emccVersion = (& $emcc --version 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $emccVersion -notmatch [regex]::Escape($webVersion)) {
    throw "HuxerUI SDK packaging requires Emscripten $webVersion"
}

Reset-OwnedDirectory $platformArtifactRoot $buildDirectory
Reset-OwnedDirectory $androidExtractDirectory $buildDirectory

$androidDirectory = Join-Path $sourceDirectory "platform/android"
$gradleWrapper = Join-Path $androidDirectory "gradlew.bat"
if (-not (Test-Path -LiteralPath $gradleWrapper -PathType Leaf)) {
    throw "HuxerUI Android Gradle wrapper is missing: $gradleWrapper"
}
$gradleVariant = $Configuration.ToLowerInvariant()
$androidNativeConfiguration = if ($Configuration -eq "Release") { "RelWithDebInfo" } else { "Debug" }
Invoke-Checked $gradleWrapper @(":HuxerUI:assemble$Configuration", "--no-daemon") $androidDirectory

$aarCandidates = @(Get-ChildItem -LiteralPath (Join-Path $androidDirectory "huxerui/build/outputs/aar") `
        -Filter "*-$gradleVariant.aar" -File)
if ($aarCandidates.Count -ne 1) {
    throw "Expected one HuxerUI Android $Configuration AAR, found $($aarCandidates.Count)"
}
Invoke-Checked $jar @("-xf", $aarCandidates[0].FullName) $androidExtractDirectory

$androidArtifactDirectory = Join-Path $platformArtifactRoot "android"
New-Item -ItemType Directory -Path $androidArtifactDirectory -Force | Out-Null
foreach ($abi in @("arm64-v8a", "x86_64")) {
    $abiOutput = Join-Path $androidArtifactDirectory $abi
    New-Item -ItemType Directory -Path $abiOutput -Force | Out-Null
    $sharedLibrary = Join-Path $androidExtractDirectory "jni/$abi/libhuxerui.so"
    if (-not (Test-Path -LiteralPath $sharedLibrary -PathType Leaf)) {
        throw "HuxerUI Android AAR is missing $abi/libhuxerui.so"
    }
    Copy-Item -LiteralPath $sharedLibrary -Destination (Join-Path $abiOutput "libhuxerui.so")

    $staticRoot = Join-Path $androidDirectory "huxerui/.cxx/$androidNativeConfiguration"
    $abiPattern = "[\\/]" + [regex]::Escape($abi) + "[\\/]lib[\\/]libhuxerui_static\.a$"
    $staticCandidates = @(
        Get-ChildItem -LiteralPath $staticRoot -Filter "libhuxerui_static.a" -File -Recurse |
            Where-Object { $_.FullName -match $abiPattern }
    )
    if ($staticCandidates.Count -ne 1) {
        throw "Expected one HuxerUI Android $Configuration static library for $abi, found $($staticCandidates.Count)"
    }
    Copy-Item -LiteralPath $staticCandidates[0].FullName -Destination (Join-Path $abiOutput "libhuxerui_static.a")
}

Remove-Item -LiteralPath (Join-Path $androidExtractDirectory "jni") -Recurse -Force
if (Test-Path -LiteralPath (Join-Path $androidExtractDirectory "prefab")) {
    Remove-Item -LiteralPath (Join-Path $androidExtractDirectory "prefab") -Recurse -Force
}
$javaOnlyAar = Join-Path $androidArtifactDirectory "HuxerUI.aar"
Invoke-Checked $jar @(
    "--create", "--file", $javaOnlyAar,
    "--no-manifest", "-C", $androidExtractDirectory, "."
) $sourceDirectory

Invoke-Checked $emcmake @(
    $cmake,
    "-S", $sourceDirectory,
    "-B", $webBuildDirectory,
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DHUXERUI_BUILD_SHARED=OFF",
    "-DHUXERUI_BUILD_STATIC=ON",
    "-DHUXERUI_BUILD_CLI=OFF",
    "-DHUXERUI_BUILD_EXAMPLES=OFF",
    "-DHUXERUI_BUILD_TESTS=OFF"
) $sourceDirectory
Invoke-Checked $cmake @(
    "--build", $webBuildDirectory,
    "--target", "huxerui_static",
    "--parallel", $Jobs
) $sourceDirectory
$webLibrary = Join-Path $webBuildDirectory "lib/libhuxerui_static.a"
if (-not (Test-Path -LiteralPath $webLibrary -PathType Leaf)) {
    throw "HuxerUI Web build did not produce libhuxerui_static.a"
}
$webArtifactDirectory = Join-Path $platformArtifactRoot "web/emscripten-$webVersion"
New-Item -ItemType Directory -Path $webArtifactDirectory -Force | Out-Null
Copy-Item -LiteralPath $webLibrary -Destination (Join-Path $webArtifactDirectory "libhuxerui.a")

$hostConfigureArguments = @(
    "-S", $sourceDirectory,
    "-B", $hostBuildDirectory,
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DHUXERUI_BUILD_CLI=ON",
    "-DHUXERUI_BUILD_EXAMPLES=OFF",
    "-DHUXERUI_BUILD_TESTS=OFF",
    "-DHUXERUI_SDK_PLATFORM_ARTIFACT_ROOT=$platformArtifactRoot"
)
if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
    $hostConfigureArguments += @("-G", "Ninja")
}
Invoke-Checked $cmake $hostConfigureArguments $sourceDirectory
Invoke-Checked $cmake @(
    "--build", $hostBuildDirectory,
    "--config", $Configuration,
    "--parallel", $Jobs
) $sourceDirectory
$packageGenerator = if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) { "ZIP" } else { "TGZ" }
Invoke-Checked $cpack @(
    "--config", (Join-Path $hostBuildDirectory "CPackConfig.cmake"),
    "-C", $Configuration,
    "-G", $packageGenerator,
    "-B", $outputDirectory
) $sourceDirectory

Write-Host "HuxerUI SDK package written to $outputDirectory"
