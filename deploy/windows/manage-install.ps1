# Used only by the per-user installer. Never searches/kills by executable name alone.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('Prepare','Remove')][string]$Action,
    [Parameter(Mandatory=$true)][string]$InstallDir,
    [switch]$KeepSettings
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
try {
    $root = [IO.Path]::GetFullPath($InstallDir).TrimEnd('\')
    if ($root -eq [IO.Path]::GetPathRoot($root).TrimEnd('\')) { throw 'A drive root is not an application folder.' }
    $cursor = $root
    while ($cursor) {
        if ((Test-Path -LiteralPath $cursor) -and
            ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing installation through a junction/symlink: $cursor"
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    $profile = Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'com.limelight\moonlight-tv'
    if ($Action -eq 'Remove' -and -not $KeepSettings -and (Test-Path -LiteralPath $profile)) {
        $items = @(Get-Item -LiteralPath ([IO.Path]::GetDirectoryName($profile)) -Force) +
            @(Get-Item -LiteralPath $profile -Force) + @(Get-ChildItem -LiteralPath $profile -Recurse -Force)
        if (@($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count -gt 0) {
            throw 'Settings contain a junction/symlink; remove it before uninstalling, or choose Keep settings.'
        }
    }
    $marker = Join-Path $root 'eclipse-install.json'
    $manifest = $null
    if (Test-Path -LiteralPath $marker -PathType Leaf) {
        $manifest = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
        if ($manifest.product -ne 'Eclipse-client' -or $manifest.schema -ne 1) { throw 'Invalid Eclipse installation marker.' }
        # Validate the entire list before deleting anything, including against reparse points.
        foreach ($relative in @($manifest.files)) {
            if ([string]::IsNullOrWhiteSpace($relative) -or [IO.Path]::IsPathRooted($relative) -or
                $relative -match '(^|[\\/])\.\.([\\/]|$)' -or $relative.Contains(':')) { throw 'Unsafe installed file list.' }
            $full = [IO.Path]::GetFullPath((Join-Path $root $relative))
            if (-not $full.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Installed file escapes application directory.' }
            $cursor = $full
            while ($cursor -and $cursor.Length -ge $root.Length) {
                if (Test-Path -LiteralPath $cursor) {
                    if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                        throw "Refusing cleanup through a junction/symlink: $cursor"
                    }
                }
                $cursor = [IO.Path]::GetDirectoryName($cursor)
            }
        }
    } elseif (Test-Path -LiteralPath $root) {
        if ($Action -eq 'Remove' -or @(Get-ChildItem -LiteralPath $root -Force).Count -ne 0) {
            throw 'Choose an empty folder or an existing Eclipse installation.'
        }
    } elseif ($Action -eq 'Remove') { throw 'Installation folder is missing.' }

    $executable = Join-Path $root 'moonlight-tv.exe'
    function Get-OwnedProcesses {
        @(Get-Process -Name moonlight-tv -ErrorAction SilentlyContinue | Where-Object {
            try { $_.Path -and [string]::Equals($_.Path, $executable, [StringComparison]::OrdinalIgnoreCase) }
            catch { $false }
        })
    }
    foreach ($process in @(Get-OwnedProcesses)) { [void]$process.CloseMainWindow() }
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (@(Get-OwnedProcesses).Count -gt 0 -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
    # A blocked/crashed UI must not make uninstall leave a running client. Exact path limits
    # this to this installation, including its same-executable co-op worker.
    foreach ($process in @(Get-OwnedProcesses)) {
        try { $process.Kill(); if (-not $process.WaitForExit(5000)) { throw 'Process did not exit.' } }
        catch { if (-not $process.HasExited) { throw } }
    }
    if (@(Get-OwnedProcesses).Count -gt 0) { throw 'Eclipse is still running; installation was not removed.' }

    if ($null -ne $manifest) {
        foreach ($relative in @($manifest.files)) {
            $full = Join-Path $root $relative
            if (Test-Path -LiteralPath $full -PathType Leaf) { Remove-Item -LiteralPath $full -Force }
        }
        # Remove only directories from the owned file list, deepest first, and only if empty.
        $dirs = @($manifest.files | ForEach-Object {
            $dir = [IO.Path]::GetDirectoryName((Join-Path $root $_))
            while ($dir -and $dir.Length -gt $root.Length) { $dir; $dir = [IO.Path]::GetDirectoryName($dir) }
        } | Sort-Object -Unique | Sort-Object Length -Descending)
        foreach ($dir in $dirs) {
            if ((Test-Path -LiteralPath $dir -PathType Container) -and @(Get-ChildItem -LiteralPath $dir -Force).Count -eq 0) {
                Remove-Item -LiteralPath $dir -Force
            }
        }
    }
    if ($Action -eq 'Remove' -and -not $KeepSettings -and (Test-Path -LiteralPath $profile)) {
        Remove-Item -LiteralPath $profile -Recurse -Force
        $profileParent = [IO.Path]::GetDirectoryName($profile)
        if (@(Get-ChildItem -LiteralPath $profileParent -Force).Count -eq 0) {
            # Shared historical namespace: remove only an empty parent, never another app's data.
            [IO.Directory]::Delete($profileParent, $false)
        }
    }
    # Retain the ownership marker on EVERY failure, so cleanup can safely be retried.
    if ($null -ne $manifest) { Remove-Item -LiteralPath $marker -Force }
    exit 0
} catch {
    Write-Error -ErrorAction Continue $_
    exit 1
}
