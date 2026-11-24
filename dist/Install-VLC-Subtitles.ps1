[CmdletBinding()]
param(
    [string] $VlcDirectory = (Join-Path $env:ProgramFiles 'VideoLAN\VLC'),
    [string] $SettingsDirectory = (Join-Path $env:APPDATA 'vlc'),
    [switch] $ModuleOnly
)

$ErrorActionPreference = 'Stop'

function Assert-CompatibleVlc([string] $Executable) {
    $program = Get-Item -LiteralPath $Executable
    if ($program.VersionInfo.FileVersion -notmatch '^3\.0\.24(?:\.0)?$') {
        throw 'The installer requires VLC 3.0.24 for 64-bit Windows.'
    }

    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($program.FullName))
    try {
        $reader.BaseStream.Position = 0x3c
        $reader.BaseStream.Position = $reader.ReadUInt32()
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw 'Use the 64-bit version of VLC 3.0.24.'
        }
    } finally {
        $reader.Dispose()
    }
}

function Assert-VlcClosed {
    if (Get-Process -Name 'vlc' -ErrorAction SilentlyContinue) {
        throw 'Close VLC completely, then run the installer again.'
    }
}

function Get-FileDigest([string] $Path) {
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($sha256.ComputeHash([IO.File]::ReadAllBytes($Path)))
    } finally {
        $sha256.Dispose()
    }
}

function Copy-PackageFile([string] $Source, [string] $Destination) {
    if ((Test-Path -LiteralPath $Destination -PathType Leaf) -and
        (Get-FileDigest $Source) -eq (Get-FileDigest $Destination)) {
        return
    }

    $stagedPath = "$Destination.$([guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::Copy($Source, $stagedPath)
        if (Test-Path -LiteralPath $Destination -PathType Leaf) {
            $backup = "$Destination.before-subtitle-install-$([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))"
            [IO.File]::Replace($stagedPath, $Destination, $backup)
        } else {
            [IO.File]::Move($stagedPath, $Destination)
        }
    } finally {
        if (Test-Path -LiteralPath $stagedPath -PathType Leaf) {
            Remove-Item -LiteralPath $stagedPath
        }
    }
}

function Set-VlcOption([string] $Text, [string] $Section, [string] $Key, [string] $Value, [string] $Newline) {
    $entry = "$Key=$Value"
    $option = [regex]::new("(?m)^#?$([regex]::Escape($Key))=[^`r`n]*")
    if ($option.IsMatch($Text)) {
        return $option.Replace($Text, [Text.RegularExpressions.MatchEvaluator] { param($match) $entry })
    }

    $header = [regex]::Match($Text, "(?m)^\[$([regex]::Escape($Section))\][ `t]*(?:`r?`n|$)")
    if ($header.Success) {
        $separator = if ($header.Value.EndsWith("`n")) { '' } else { $Newline }
        return $Text.Insert($header.Index + $header.Length, "$separator$entry$Newline")
    }
    return "$Text$Newline[$Section]$Newline$entry$Newline"
}

function Enable-LiveSubtitles([string] $ConfigPath, [string] $SkinPath) {
    $original = if (Test-Path -LiteralPath $ConfigPath -PathType Leaf) { [IO.File]::ReadAllText($ConfigPath) } else { '' }
    $newline = if ($original.Contains("`r`n")) { "`r`n" } else { "`n" }
    $configured = [regex]::Matches($original, '(?m)^extraintf=([^\r\n]*)')
    $interfaces = if ($configured.Count) { $configured[$configured.Count - 1].Groups[1].Value } else { '' }
    if ($interfaces -notmatch '(?:^|:)subtitlepanel(?:$|:)') {
        $interfaces = if ($interfaces) { "$($interfaces.TrimEnd(':')):subtitlepanel" } else { 'subtitlepanel' }
    }

    $configuredFilters = [regex]::Matches($original, '(?m)^demux-filter=([^\r\n]*)')
    $filters = if ($configuredFilters.Count) { $configuredFilters[$configuredFilters.Count - 1].Groups[1].Value } else { '' }
    if ($filters -notmatch '(?:^|:)redesignsubtitlespeed(?:$|[{:])') {
        $filters = if ($filters) { "redesignsubtitlespeed:$filters" } else { 'redesignsubtitlespeed' }
    }

    $updated = Set-VlcOption $original 'core' 'intf' 'skins2' $newline
    $updated = Set-VlcOption $updated 'core' 'extraintf' $interfaces $newline
    $updated = Set-VlcOption $updated 'core' 'demux-filter' $filters $newline
    $updated = Set-VlcOption $updated 'skins2' 'skins2-last' $SkinPath $newline
    if ($updated -ceq $original) { return }

    $stagedPath = "$ConfigPath.$([guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText($stagedPath, $updated, [Text.UTF8Encoding]::new($false))
        if (Test-Path -LiteralPath $ConfigPath -PathType Leaf) {
            $backup = "$ConfigPath.before-live-subtitles-$([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))"
            [IO.File]::Replace($stagedPath, $ConfigPath, $backup)
        } else {
            [IO.File]::Move($stagedPath, $ConfigPath)
        }
    } finally {
        if (Test-Path -LiteralPath $stagedPath -PathType Leaf) {
            Remove-Item -LiteralPath $stagedPath
        }
    }
}

try {
    $VlcDirectory = (Resolve-Path -LiteralPath $VlcDirectory).Path
    $SettingsDirectory = [IO.Path]::GetFullPath($SettingsDirectory)
    Assert-CompatibleVlc (Join-Path $VlcDirectory 'vlc.exe')
    Assert-VlcClosed

    $moduleName = 'libsubtitlepanel_plugin.dll'
    $skinName = 'VLC-Redesign.vlt'
    $moduleSource = Join-Path $PSScriptRoot $moduleName
    $skinSource = Join-Path $PSScriptRoot $skinName
    $modulePath = Join-Path $VlcDirectory "plugins\gui\$moduleName"
    foreach ($payload in @($moduleSource, $skinSource)) {
        if (!(Test-Path -LiteralPath $payload -PathType Leaf)) {
            throw 'Keep the installer scripts, DLL, and skin in the same folder. If you downloaded a ZIP archive, extract it first.'
        }
    }

    if ($ModuleOnly) {
        Copy-PackageFile $moduleSource $modulePath
        exit 0
    }

    try {
        Copy-PackageFile $moduleSource $modulePath
    } catch [UnauthorizedAccessException] {
        $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath),
                       '-VlcDirectory', ('"{0}"' -f $VlcDirectory), '-ModuleOnly')
        $elevation = Start-Process -FilePath $powershell -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru -Wait
        if ($elevation.ExitCode -ne 0) { throw 'Windows could not install the VLC module.' }
    }

    if ((Get-FileDigest $moduleSource) -ne (Get-FileDigest $modulePath)) {
        throw 'The installed VLC module does not match the installer package.'
    }
    Assert-VlcClosed
    $skinDirectory = Join-Path $SettingsDirectory 'skins'
    [IO.Directory]::CreateDirectory($skinDirectory) | Out-Null
    $skinPath = Join-Path $skinDirectory $skinName
    Copy-PackageFile $skinSource $skinPath
    Enable-LiveSubtitles (Join-Path $SettingsDirectory 'vlcrc') $skinPath
    [Console]::WriteLine('Installation complete. Start VLC from your usual shortcut.')
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
