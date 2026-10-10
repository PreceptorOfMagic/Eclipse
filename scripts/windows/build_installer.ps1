<# Build a per-user NSIS installer from a verified portable package. No runtime downloads. #>
[CmdletBinding()]
param(
    [string]$PackageDir = (Join-Path $PSScriptRoot '..\..\dist\eclipse-windows-x64'),
    [string]$Makensis = 'makensis.exe',
    # The signed release (finalize_signed_package.ps1) drops the -unsigned suffix.
    [string]$OutputName = 'eclipse-windows-x64-setup-unsigned.exe'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$PackageDir = (Resolve-Path -LiteralPath $PackageDir).Path
$compiler = (Get-Command $Makensis -ErrorAction Stop).Source
$versionMatch = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(moonlight VERSION (\d+\.\d+\.\d+)')
if (-not $versionMatch.Success) { throw 'Cannot determine Eclipse version.' }
$version = $versionMatch.Groups[1].Value
foreach ($required in @('moonlight-tv.exe','LICENSE.txt','COPYRIGHT','ACKNOWLEDGEMENTS.md','SOURCE-MANIFEST.txt','THIRD-PARTY-NOTICES.txt','SHA256SUMS')) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageDir $required) -PathType Leaf)) { throw "Package missing $required" }
}
$recorded = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $PackageDir 'SHA256SUMS')) {
    if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$') { throw "Invalid package checksum line: $line" }
    $expected = $Matches[1]; $relative = $Matches[2]
    $path = [IO.Path]::GetFullPath((Join-Path $PackageDir $relative))
    if (-not $path.StartsWith($PackageDir + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Checksum path escapes package.' }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $expected) { throw "Package checksum failed: $relative" }
    $recorded[$relative.Replace('\','/')] = $true
}
foreach ($file in Get-ChildItem -LiteralPath $PackageDir -Recurse -File) {
    $relative = $file.FullName.Substring($PackageDir.Length + 1).Replace('\','/')
    if ($relative -ne 'SHA256SUMS' -and -not $recorded.ContainsKey($relative)) { throw "Untracked package payload: $relative" }
}
$stage = Join-Path $root 'build\windows-installer\payload'
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -Path $stage -ItemType Directory -Force | Out-Null
Get-ChildItem -LiteralPath $PackageDir -Force | Copy-Item -Destination $stage -Recurse -Force
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object { $_.FullName.Substring($stage.Length + 1) })
@{ product='Eclipse-client'; schema=1; version=$version; files=$files } | ConvertTo-Json -Depth 3 |
    Set-Content -Encoding UTF8 -LiteralPath (Join-Path $stage 'eclipse-install.json')
$output = Join-Path $root (Join-Path 'dist' $OutputName)
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $output) | Out-Null
& $compiler /V2 "/DVERSION=$version" "/DOUTPUT_FILE=$output" "/DPAYLOAD_DIR=$stage" "/DHELPER_FILE=$root\deploy\windows\manage-install.ps1" "$root\deploy\windows\eclipse.nsi"
if ($LASTEXITCODE -ne 0) { throw "NSIS failed: $LASTEXITCODE" }
$digest = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
"$digest  $([IO.Path]::GetFileName($output))" | Set-Content -Encoding ASCII -LiteralPath "$output.sha256"
Write-Host "Installer: $output"
