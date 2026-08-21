[CmdletBinding()]
param(
    [string]$Version,
    [string]$Prefix,
    [string]$Archive,
    [switch]$Yes,
    [switch]$Uninstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepositoryUrl = "https://github.com/HuxerUI/HuxerUI"

function Fail([string]$Message) {
    throw "HuxerUI installer: $Message"
}

function Test-HuxerUISdk([string]$Root) {
    return (Test-Path -LiteralPath (Join-Path $Root "bin/huxerui.exe") -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $Root "include/huxerui/huxerui.h") -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $Root "lib/cmake/HuxerUI/HuxerUIConfig.cmake") -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $Root "share/huxerui/resources/huxerui/resources.bin") -PathType Leaf)
}

function Confirm-Action([string]$Description) {
    Write-Host $Description
    if ($Yes) {
        return
    }
    $Answer = Read-Host "Continue? [y/N]"
    if ($Answer -notmatch "^(?i:y|yes)$") {
        Fail "cancelled"
    }
}

function Assert-SafePrefix([string]$Path) {
    $Root = [System.IO.Path]::GetPathRoot($Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or
        $Path.Equals($Root, [System.StringComparison]::OrdinalIgnoreCase) -or
        $Path.Equals($env:USERPROFILE, [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail "unsafe installation prefix: $Path"
    }
}

function Set-UserEnvironment([string]$SdkRoot) {
    $OldHome = [Environment]::GetEnvironmentVariable("HUXERUI_HOME", "User")
    $OldPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $Entries = [System.Collections.Generic.List[string]]::new()
    if ($OldPath) {
        foreach ($Entry in $OldPath.Split(";", [System.StringSplitOptions]::RemoveEmptyEntries)) {
            if ($OldHome -and $Entry.Equals((Join-Path $OldHome "bin"), [System.StringComparison]::OrdinalIgnoreCase)) {
                continue
            }
            if (-not $Entry.Equals((Join-Path $SdkRoot "bin"), [System.StringComparison]::OrdinalIgnoreCase)) {
                $Entries.Add($Entry)
            }
        }
    }
    $Entries.Add((Join-Path $SdkRoot "bin"))
    try {
        [Environment]::SetEnvironmentVariable("HUXERUI_HOME", $SdkRoot, "User")
        [Environment]::SetEnvironmentVariable("Path", ($Entries -join ";"), "User")
    } catch {
        [Environment]::SetEnvironmentVariable("HUXERUI_HOME", $OldHome, "User")
        [Environment]::SetEnvironmentVariable("Path", $OldPath, "User")
        throw
    }
}

function Remove-UserEnvironment([string]$SdkRoot) {
    $CurrentHome = [Environment]::GetEnvironmentVariable("HUXERUI_HOME", "User")
    if (-not $CurrentHome -or
        -not $CurrentHome.Equals($SdkRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        return
    }
    $CurrentPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $Entries = [System.Collections.Generic.List[string]]::new()
    if ($CurrentPath) {
        foreach ($Entry in $CurrentPath.Split(";", [System.StringSplitOptions]::RemoveEmptyEntries)) {
            if (-not $Entry.Equals((Join-Path $SdkRoot "bin"), [System.StringComparison]::OrdinalIgnoreCase)) {
                $Entries.Add($Entry)
            }
        }
    }
    [Environment]::SetEnvironmentVariable("HUXERUI_HOME", $null, "User")
    [Environment]::SetEnvironmentVariable("Path", ($Entries -join ";"), "User")
}

if ($Uninstall -and ($Version -or $Archive)) {
    Fail "-Uninstall cannot be combined with -Version or -Archive"
}
if ($Version -and $Archive) {
    Fail "-Version and -Archive cannot be combined"
}
if (-not $env:LOCALAPPDATA) {
    Fail "LOCALAPPDATA is not configured"
}
if (-not $Prefix) {
    $Prefix = Join-Path $env:LOCALAPPDATA "HuxerUI"
}
$Prefix = [System.IO.Path]::GetFullPath($Prefix)
Assert-SafePrefix $Prefix

if ($Uninstall) {
    if (-not (Test-HuxerUISdk $Prefix)) {
        Fail "refusing to remove a directory that is not a HuxerUI SDK: $Prefix"
    }
    Confirm-Action "Uninstall HuxerUI SDK`n  SDK: $Prefix"
    Remove-UserEnvironment $Prefix
    Remove-Item -LiteralPath $Prefix -Recurse -Force
    Write-Host "HuxerUI SDK removed from $Prefix"
    exit 0
}

$Architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
if ($Architecture -notin @("x64", "x86_64")) {
    Fail "Windows SDK archives currently support x86_64 hosts only"
}
$HostArchitecture = "x86_64"

$ReleaseTag = $null
if (-not $Archive) {
    if (-not $Version) {
        $Release = Invoke-RestMethod -Uri "$RepositoryUrl/releases/latest"
        $ReleaseTag = [string]$Release.tag_name
        if (-not $ReleaseTag.StartsWith("v")) {
            Fail "latest HuxerUI release has an invalid tag: $ReleaseTag"
        }
        $Version = $ReleaseTag.Substring(1)
    } else {
        $Version = $Version.TrimStart("v")
        $ReleaseTag = "v$Version"
    }
    if ($Version -notmatch "^[0-9A-Za-z._-]+$") {
        Fail "invalid version: $Version"
    }
    $ArchiveName = "huxerui-sdk-$Version-windows-$HostArchitecture.zip"
    $ArchiveSource = "$RepositoryUrl/releases/download/$ReleaseTag/$ArchiveName"
    $ArchiveDisplay = $ArchiveSource
} else {
    $Archive = [System.IO.Path]::GetFullPath($Archive)
    if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) {
        Fail "archive does not exist: $Archive"
    }
    $ArchiveName = Split-Path -Leaf $Archive
    $ArchiveDisplay = $Archive
}

if ($ArchiveName -notmatch "^huxerui-sdk-.+-windows-x86_64\.zip$") {
    Fail "archive does not match this host: $ArchiveName"
}
if ((Test-Path -LiteralPath $Prefix) -and -not (Test-HuxerUISdk $Prefix)) {
    Fail "installation prefix exists but is not a HuxerUI SDK: $Prefix"
}

Confirm-Action "Install HuxerUI SDK`n  Archive: $ArchiveDisplay`n  SDK: $Prefix"

$TemporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("huxerui-sdk-" + [guid]::NewGuid())
$StagingDirectory = $null
$BackupDirectory = $null
New-Item -ItemType Directory -Path $TemporaryDirectory | Out-Null
try {
    if (-not $Archive) {
        $Archive = Join-Path $TemporaryDirectory $ArchiveName
        Invoke-WebRequest -Uri $ArchiveSource -OutFile $Archive -UseBasicParsing
        Invoke-WebRequest -Uri "$ArchiveSource.sha256" -OutFile "$Archive.sha256" -UseBasicParsing
    }

    $ChecksumPath = "$Archive.sha256"
    if (-not (Test-Path -LiteralPath $ChecksumPath -PathType Leaf)) {
        Fail "archive checksum does not exist: $ChecksumPath"
    }
    $ChecksumParts = ((Get-Content -LiteralPath $ChecksumPath -TotalCount 1).Trim() -split "\s+")
    if ($ChecksumParts.Count -lt 2 -or $ChecksumParts[1] -ne $ArchiveName) {
        Fail "archive checksum is malformed or names a different file: $ChecksumPath"
    }
    $ActualChecksum = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash
    if (-not $ActualChecksum.Equals($ChecksumParts[0], [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail "archive checksum does not match: $ArchiveName"
    }

    $ExtractDirectory = Join-Path $TemporaryDirectory "extract"
    Expand-Archive -LiteralPath $Archive -DestinationPath $ExtractDirectory
    $ArchiveRoot = $ArchiveName.Substring(0, $ArchiveName.Length - ".zip".Length)
    $ExtractedSdk = Join-Path $ExtractDirectory $ArchiveRoot
    if (-not (Test-HuxerUISdk $ExtractedSdk)) {
        Fail "archive does not contain a complete HuxerUI SDK"
    }

    $Parent = Split-Path -Parent $Prefix
    New-Item -ItemType Directory -Path $Parent -Force | Out-Null
    $StagingDirectory = Join-Path $Parent (".huxerui-install-" + [guid]::NewGuid())
    Move-Item -LiteralPath $ExtractedSdk -Destination $StagingDirectory
    if (Test-Path -LiteralPath $Prefix) {
        $BackupDirectory = Join-Path $Parent (".huxerui-backup-" + [guid]::NewGuid())
        Move-Item -LiteralPath $Prefix -Destination $BackupDirectory
    }
    try {
        Move-Item -LiteralPath $StagingDirectory -Destination $Prefix
        $StagingDirectory = $null
        Set-UserEnvironment $Prefix
    } catch {
        Remove-Item -LiteralPath $Prefix -Recurse -Force -ErrorAction SilentlyContinue
        if ($BackupDirectory -and (Test-Path -LiteralPath $BackupDirectory)) {
            Move-Item -LiteralPath $BackupDirectory -Destination $Prefix
            $BackupDirectory = $null
        }
        throw
    }

    if ($BackupDirectory -and (Test-Path -LiteralPath $BackupDirectory)) {
        Remove-Item -LiteralPath $BackupDirectory -Recurse -Force
        $BackupDirectory = $null
    }
} finally {
    if ($StagingDirectory -and (Test-Path -LiteralPath $StagingDirectory)) {
        Remove-Item -LiteralPath $StagingDirectory -Recurse -Force
    }
    if ($BackupDirectory -and (Test-Path -LiteralPath $BackupDirectory)) {
        Write-Warning "Previous HuxerUI SDK remains at $BackupDirectory"
    }
    if (Test-Path -LiteralPath $TemporaryDirectory) {
        Remove-Item -LiteralPath $TemporaryDirectory -Recurse -Force
    }
}

Write-Host "HuxerUI SDK installed at $Prefix"
Write-Host "Restart the terminal to use the updated user environment."
