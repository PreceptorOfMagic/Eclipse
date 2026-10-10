<#
.SYNOPSIS
Build the native LVGL Windows client and assemble a portable x64 package.

.DESCRIPTION
Uses the MinGW64 environment supplied by MSYS2. The script installs the
declared build packages, configures and builds with CMake/Ninja, stages via
cmake --install, then follows each PE import recursively and copies only the
required MinGW runtime DLLs. Windows system DLLs are never redistributed.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',

    [string]$MsysRoot = $(if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }),

    [switch]$Clean,
    [switch]$SkipPackageInstall,
    [switch]$NoArchive,

    # CI and release builds must start from an exactly reproducible checkout.
    # Local smoke builds remain usable from a dirty tree and record that state.
    [switch]$RequireCleanTree
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$BuildParallelArguments = '--parallel'
if ($env:CMAKE_BUILD_PARALLEL_LEVEL) {
    $BuildParallelJobs = 0
    if (-not [int]::TryParse($env:CMAKE_BUILD_PARALLEL_LEVEL, [ref]$BuildParallelJobs) -or
        $env:CMAKE_BUILD_PARALLEL_LEVEL -notmatch '^[1-9][0-9]*$' -or $BuildParallelJobs -le 0) {
        throw 'CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer.'
    }
    $BuildParallelArguments += " $BuildParallelJobs"
}

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$MsysRoot = (Resolve-Path $MsysRoot).Path
$Bash = Join-Path $MsysRoot 'usr\bin\bash.exe'
$Cygpath = Join-Path $MsysRoot 'usr\bin\cygpath.exe'
$Pacman = Join-Path $MsysRoot 'usr\bin\pacman.exe'
$MingwBin = Join-Path $MsysRoot 'mingw64\bin'
$BuildDir = Join-Path $ProjectRoot 'build\windows-x64'
$StageDir = Join-Path $BuildDir 'stage'
$PackageDir = Join-Path $ProjectRoot 'dist\eclipse-windows-x64'
$Archive = Join-Path $ProjectRoot 'dist\eclipse-windows-x64-unsigned.zip'
$ArchiveHash = "$Archive.sha256"
$ControllerDbRevision = '5a12daa568d19344f9b6e9286ef5929833b25c7c'
$ControllerDbSha256 = '07ec5b753e685c4829987919b27905cc3c45c8e18c11b3046e275fcf38b0f3cb'
$ControllerDbUrl = "https://raw.githubusercontent.com/gabomdq/SDL_GameControllerDB/$ControllerDbRevision/gamecontrollerdb.txt"
$ControllerDbLicenseSha256 = '97f35b302b361680ec1e891e95d2d52097bb95abff361434916d99dc1305f127'
$ControllerDbLicenseUrl = "https://raw.githubusercontent.com/gabomdq/SDL_GameControllerDB/$ControllerDbRevision/LICENSE"
$MaterialIconsRevision = '27e9ef1dbeedc13d682fece4a58e1eda4cb0961a'
$MaterialIconsLicenseSha256 = '58d1e17ffe5109a7ae296caafcadfdbe6a7d176f0bc4ab01e12a689b0499d8bd'
$MaterialIconsLicenseUrl = "https://raw.githubusercontent.com/google/material-design-icons/$MaterialIconsRevision/LICENSE"
$DownloadDir = Join-Path $BuildDir 'downloads'
$ControllerDbSource = Join-Path $DownloadDir "gamecontrollerdb-$ControllerDbRevision.txt"
$ControllerDbLicenseSource = Join-Path $DownloadDir "SDL_GameControllerDB-LICENSE-$ControllerDbRevision.txt"
$MaterialIconsLicenseSource = Join-Path $DownloadDir "Material-Icons-LICENSE-$MaterialIconsRevision.txt"

foreach ($requiredPath in @($Bash, $Cygpath, $Pacman, $MingwBin)) {
    if (-not (Test-Path $requiredPath)) {
        throw "MSYS2 MinGW64 was not found at '$MsysRoot'. Install MSYS2 or pass -MsysRoot."
    }
}

function Convert-ToMsysPath([string]$Path) {
    $converted = & $Cygpath -u $Path
    if ($LASTEXITCODE -ne 0) {
        throw "cygpath failed for '$Path'."
    }
    return $converted.Trim()
}

function Quote-Bash([string]$Value) {
    $embeddedQuote = "'" + [char]34 + "'" + [char]34 + "'"
    return "'" + $Value.Replace("'", $embeddedQuote) + "'"
}

function Invoke-Mingw([string]$Command) {
    $oldMsystem = $env:MSYSTEM
    $oldChere = $env:CHERE_INVOKING
    try {
        $env:MSYSTEM = 'MINGW64'
        $env:CHERE_INVOKING = '1'
        & $Bash --login -c "export PATH=/mingw64/bin:/usr/bin:`$PATH; $Command"
        if ($LASTEXITCODE -ne 0) {
            throw "MSYS2 command failed with exit code $LASTEXITCODE."
        }
    } finally {
        $env:MSYSTEM = $oldMsystem
        $env:CHERE_INVOKING = $oldChere
    }
}

function Invoke-MingwCapture([string]$Command) {
    $oldMsystem = $env:MSYSTEM
    $oldChere = $env:CHERE_INVOKING
    try {
        $env:MSYSTEM = 'MINGW64'
        $env:CHERE_INVOKING = '1'
        $output = @(& $Bash --login -c "export PATH=/mingw64/bin:/usr/bin:`$PATH; $Command")
        if ($LASTEXITCODE -ne 0) {
            throw "MSYS2 command failed with exit code $LASTEXITCODE."
        }
        return $output
    } finally {
        $env:MSYSTEM = $oldMsystem
        $env:CHERE_INVOKING = $oldChere
    }
}

function Get-VerifiedDownload([string]$Uri, [string]$ExpectedSha256, [string]$Destination,
                              [string]$Description) {
    if (Test-Path $Destination) {
        $cachedHash = (Get-FileHash -Algorithm SHA256 $Destination).Hash.ToLowerInvariant()
        if ($cachedHash -eq $ExpectedSha256) {
            return
        }
    }

    $download = Join-Path $DownloadDir ("verified-{0}.download" -f [Guid]::NewGuid().ToString('N'))
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $download
        $downloadHash = (Get-FileHash -Algorithm SHA256 $download).Hash.ToLowerInvariant()
        if ($downloadHash -ne $ExpectedSha256) {
            throw "$Description hash mismatch: expected $ExpectedSha256, got $downloadHash."
        }
        Move-Item -Force $download $Destination
    } finally {
        if (Test-Path $download) {
            Remove-Item -Force $download
        }
    }
}

$Packages = @(
    'git',
    'mingw-w64-x86_64-binutils',
    'mingw-w64-x86_64-ca-certificates',
    'mingw-w64-x86_64-cmake',
    'mingw-w64-x86_64-curl',
    'mingw-w64-x86_64-expat',
    'mingw-w64-x86_64-ffmpeg',
    'mingw-w64-x86_64-fontconfig',
    'mingw-w64-x86_64-freetype',
    'mingw-w64-x86_64-gcc',
    'mingw-w64-x86_64-gettext-tools',
    'mingw-w64-x86_64-mbedtls',
    'mingw-w64-x86_64-ninja',
    'mingw-w64-x86_64-opus',
    'mingw-w64-x86_64-pkgconf',
    'mingw-w64-x86_64-SDL2',
    'mingw-w64-x86_64-SDL2_image'
)

$Git = Get-Command git -ErrorAction SilentlyContinue
if ($null -eq $Git) {
    throw 'Git is required to build from an Eclipse source checkout.'
}

# Inspect the checkout before bootstrap_client_deps.sh deliberately applies the
# repository-owned patches inside three public submodules. Those expected patch
# applications make the post-bootstrap submodule worktrees dirty by design.
$Commit = (& $Git.Source -C $ProjectRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or -not $Commit) {
    throw "'$ProjectRoot' is not a valid Git checkout."
}
$InitialDirtyEntries = @(& $Git.Source -C $ProjectRoot status --porcelain=v1 --untracked-files=all --ignore-submodules=none)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to inspect the source checkout state.'
}
$SourceWasDirty = $InitialDirtyEntries.Count -gt 0
if ($RequireCleanTree -and $SourceWasDirty) {
    $preview = ($InitialDirtyEntries | Select-Object -First 20) -join "`n"
    throw "Release packaging requires a clean checkout. Preserve or commit these changes first:`n$preview"
}

if (-not $SkipPackageInstall) {
    Invoke-Mingw ("pacman --noconfirm --needed -S " + ($Packages -join ' '))
}

$SourceMsysPath = Convert-ToMsysPath $ProjectRoot
$SourceMsys = Quote-Bash $SourceMsysPath
Invoke-Mingw "cd $SourceMsys && ./scripts/bootstrap_client_deps.sh"

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}
if (Test-Path $StageDir) {
    Remove-Item -Recurse -Force $StageDir
}
if (Test-Path $PackageDir) {
    Remove-Item -Recurse -Force $PackageDir
}
if (Test-Path $Archive) {
    Remove-Item -Force $Archive
}
if (Test-Path $ArchiveHash) {
    Remove-Item -Force $ArchiveHash
}
New-Item -ItemType Directory -Force -Path $BuildDir, $StageDir, $PackageDir, $DownloadDir | Out-Null

# Ship a known community controller database on first launch instead of making
# controller support depend on the asynchronous updater. Keep the upstream
# revision and content hash together here: a changed or compromised response is
# rejected, while an already-verified cache permits repeat builds offline. Its
# upstream licence is pinned and shipped alongside it rather than being lost at
# the packaging boundary.
Get-VerifiedDownload $ControllerDbUrl $ControllerDbSha256 $ControllerDbSource 'Controller database'
Get-VerifiedDownload $ControllerDbLicenseUrl $ControllerDbLicenseSha256 $ControllerDbLicenseSource 'Controller database licence'
Get-VerifiedDownload $MaterialIconsLicenseUrl $MaterialIconsLicenseSha256 $MaterialIconsLicenseSource 'Material Icons licence'

$BuildMsysPath = Convert-ToMsysPath $BuildDir
$StageMsysPath = Convert-ToMsysPath $StageDir
$BuildMsys = Quote-Bash $BuildMsysPath
$StageMsys = Quote-Bash $StageMsysPath

# __FILE__ and debug records must not disclose the builder's account or checkout
# location. Cover both forms MinGW can hand to GCC: Windows paths with forward
# slashes and MSYS paths. Keep these in ordinary CMAKE_*_FLAGS so project and
# dependency targets receive the same mapping.
$PrefixMappings = @(
    @{ From = $BuildDir.Replace('\', '/'); To = '/build' },
    @{ From = $BuildMsysPath; To = '/build' },
    @{ From = $ProjectRoot.Replace('\', '/'); To = '/src' },
    @{ From = $SourceMsysPath; To = '/src' }
)
$PrefixMapFlags = @()
foreach ($mapping in $PrefixMappings) {
    $escapedFrom = $mapping.From.Replace('"', '\"')
    foreach ($kind in @('file', 'macro', 'debug')) {
        $PrefixMapFlags += "-f${kind}-prefix-map=`"$escapedFrom`"=$($mapping.To)"
    }
}
$PrefixMapFlags = $PrefixMapFlags -join ' '
$CFlagsArgument = Quote-Bash ("-DCMAKE_C_FLAGS=$PrefixMapFlags")
$CxxFlagsArgument = Quote-Bash ("-DCMAKE_CXX_FLAGS=$PrefixMapFlags")

Invoke-Mingw @"
cmake -S $SourceMsys -B $BuildMsys -G Ninja \
  -DCMAKE_BUILD_TYPE=$Configuration \
  -DBUILD_TESTS=OFF \
  -DINIH_STATIC=ON \
  $CFlagsArgument \
  $CxxFlagsArgument
cmake --build $BuildMsys $BuildParallelArguments
cmake --install $BuildMsys --prefix $StageMsys
"@

# The SS4S Windows loader intentionally resolves modules from the process
# directory. Flatten CMake's bin/lib install split into the portable directory.
foreach ($installPart in @('bin', 'lib')) {
    $source = Join-Path $StageDir $installPart
    if (Test-Path $source) {
        # The lib directory also contains static development archives.  They are
        # build products, not runtime dependencies, and should not inflate or
        # confuse the portable end-user package.
        Get-ChildItem -File $source |
            Where-Object { $_.Extension -ne '.a' } |
            Copy-Item -Destination $PackageDir -Force
    }
}

$InstalledShare = Join-Path $StageDir 'share'
if (Test-Path $InstalledShare) {
    $PackageShare = Join-Path $PackageDir 'share'
    New-Item -ItemType Directory -Force -Path $PackageShare | Out-Null
    Get-ChildItem -Force $InstalledShare | Copy-Item -Recurse -Force -Destination $PackageShare
}

Copy-Item -Force $ControllerDbSource (Join-Path $PackageDir 'gamecontrollerdb.txt')
$LicenseDir = Join-Path $PackageDir 'licenses'
New-Item -ItemType Directory -Force -Path $LicenseDir | Out-Null
$SourceLicenseDir = Join-Path $LicenseDir 'source'
$ExternalLicenseDir = Join-Path $LicenseDir 'external'
New-Item -ItemType Directory -Force -Path $SourceLicenseDir, $ExternalLicenseDir | Out-Null
Copy-Item -Force (Join-Path $ProjectRoot 'LICENSE.txt') (Join-Path $PackageDir 'LICENSE.txt')
Copy-Item -Force (Join-Path $ProjectRoot 'COPYRIGHT') (Join-Path $PackageDir 'COPYRIGHT')
Copy-Item -Force (Join-Path $ProjectRoot 'ACKNOWLEDGEMENTS.md') (Join-Path $PackageDir 'ACKNOWLEDGEMENTS.md')
Copy-Item -Force $ControllerDbLicenseSource (Join-Path $ExternalLicenseDir 'SDL_GameControllerDB-LICENSE.txt')
Copy-Item -Force $MaterialIconsLicenseSource (Join-Path $ExternalLicenseDir 'Material-Icons-Apache-2.0.txt')

$ProjectNoticeSpecs = @(
    @{ Name = 'SDL2 embedded component notices';
       Path = (Join-Path $ProjectRoot 'docs\licenses\webos\SDL2-THIRD-PARTY-NOTICES.txt');
       File = 'SDL2-THIRD-PARTY-NOTICES.txt'; Directory = $ExternalLicenseDir },
    @{ Name = 'libmicrodns compatibility implementation';
       Path = (Join-Path $ProjectRoot 'docs\licenses\webos\libmicrodns-compat-ISC-style.txt');
       File = 'libmicrodns-compat-ISC-style.txt'; Directory = $SourceLicenseDir },
    @{ Name = 'Mozilla public licence for CA data';
       Path = (Join-Path $ProjectRoot 'docs\licenses\common\MPL-2.0.txt');
       File = 'MPL-2.0.txt'; Directory = $ExternalLicenseDir },
    @{ Name = 'Moonlight Common C Reed-Solomon implementation';
       Path = (Join-Path $ProjectRoot 'docs\licenses\webos\Moonlight-Common-C-Reed-Solomon-BSD.txt');
       File = 'Moonlight-Common-C-Reed-Solomon-BSD.txt'; Directory = $SourceLicenseDir },
    @{ Name = 'Moonlight Qt derivative modification notice';
       Path = (Join-Path $ProjectRoot 'docs\licenses\windows\Moonlight-Qt-Derivative-NOTICE.md');
       File = 'Moonlight-Qt-Derivative-NOTICE.md'; Directory = $SourceLicenseDir },
    @{ Name = 'LVGL embedded-font notice';
       Path = (Join-Path $ProjectRoot 'docs\licenses\common\LVGL-Embedded-Fonts-NOTICE.md');
       File = 'LVGL-Embedded-Fonts-NOTICE.md'; Directory = $ExternalLicenseDir },
    @{ Name = 'Montserrat 7.200 OFL';
       Path = (Join-Path $ProjectRoot 'docs\licenses\common\Montserrat-7.200-OFL-1.1.txt');
       File = 'Montserrat-7.200-OFL-1.1.txt'; Directory = $ExternalLicenseDir },
    @{ Name = 'Font Awesome Free 5.9.0 upstream licence and brand warning';
       Path = (Join-Path $ProjectRoot 'docs\licenses\common\Font-Awesome-Free-5.9.0-LICENSE.txt');
       File = 'Font-Awesome-Free-5.9.0-LICENSE.txt'; Directory = $ExternalLicenseDir },
    @{ Name = 'SIL Open Font License 1.1';
       Path = (Join-Path $ProjectRoot 'docs\licenses\common\SIL-OFL-1.1.txt');
       File = 'SIL-OFL-1.1.txt'; Directory = $ExternalLicenseDir },
    @{ Name = 'Unscii-8 1.0 provenance';
       Path = (Join-Path $ProjectRoot 'docs\licenses\common\Unscii-8-1.0-PROVENANCE.txt');
       File = 'Unscii-8-1.0-PROVENANCE.txt'; Directory = $ExternalLicenseDir }
)
foreach ($spec in $ProjectNoticeSpecs) {
    if (-not (Test-Path -LiteralPath $spec.Path -PathType Leaf)) {
        throw "Required notice for $($spec.Name) was not found at '$($spec.Path)'."
    }
    Copy-Item -LiteralPath $spec.Path -Destination (Join-Path $spec.Directory $spec.File) -Force
}

$SourceLicenseSpecs = @(
    @{ Name = 'libmicrodns'; Source = 'https://github.com/videolabs/libmicrodns';
       Path = (Join-Path $ProjectRoot 'third_party\libmicrodns\COPYING'); File = 'libmicrodns-LGPL-2.1.txt' },
    @{ Name = 'libmicrodns authors'; Source = 'https://github.com/videolabs/libmicrodns';
       Path = (Join-Path $ProjectRoot 'third_party\libmicrodns\AUTHORS'); File = 'libmicrodns-AUTHORS.txt' },
    @{ Name = 'moonlight-common-c'; Source = 'https://github.com/moonlight-stream/moonlight-common-c';
       Path = (Join-Path $ProjectRoot 'core\moonlight-common-c\LICENSE.txt'); File = 'moonlight-common-c-GPL-3.0.txt' },
    @{ Name = 'Moonlight Qt-derived desktop video module';
       Source = 'https://github.com/moonlight-stream/moonlight-qt/tree/032529d782242e3833e0b3b147dbbf96e878e3ca';
       Path = (Join-Path $ProjectRoot 'LICENSE.txt'); File = 'Moonlight-Qt-GPL-3.0.txt' },
    @{ Name = 'ENet'; Source = 'https://github.com/lsalzman/enet';
       Path = (Join-Path $ProjectRoot 'core\moonlight-common-c\enet\LICENSE'); File = 'ENet-MIT.txt' },
    @{ Name = 'SS4S'; Source = 'https://github.com/mariotaku/ss4s';
       Path = (Join-Path $ProjectRoot 'third_party\ss4s\LICENSE'); File = 'SS4S-LGPL-3.0.txt' },
    @{ Name = 'commons-c'; Source = 'https://github.com/mariotaku/commons-c';
       Path = (Join-Path $ProjectRoot 'third_party\commons\LICENSE'); File = 'commons-c-MIT.txt' },
    @{ Name = 'LVGL'; Source = 'https://github.com/lvgl/lvgl';
       Path = (Join-Path $ProjectRoot 'third_party\lvgl\LICENCE.txt'); File = 'LVGL-MIT.txt' },
    @{ Name = 'h264bitstream'; Source = 'https://github.com/aizvorski/h264bitstream';
       Path = (Join-Path $ProjectRoot 'third_party\h264bitstream\LICENSE'); File = 'h264bitstream-LGPL-2.1.txt' }
)

$InihLicensePath = Join-Path $BuildDir 'ext_inih-prefix\src\ext_inih\LICENSE.txt'
if (Test-Path -LiteralPath $InihLicensePath -PathType Leaf) {
    $SourceLicenseSpecs += @{
        Name = 'inih r58'; Source = 'https://github.com/benhoyt/inih/tree/r58';
        Path = $InihLicensePath; File = 'inih-BSD-3-Clause.txt'
    }
}

foreach ($spec in $SourceLicenseSpecs) {
    if (-not (Test-Path -LiteralPath $spec.Path -PathType Leaf)) {
        throw "Required source licence for $($spec.Name) was not found at '$($spec.Path)'."
    }
    Copy-Item -LiteralPath $spec.Path -Destination (Join-Path $SourceLicenseDir $spec.File) -Force
}

# Filled from the files actually copied below. This is deliberately not a
# hand-maintained package list: FFmpeg's codec stack and SDL_image pull in a
# changing transitive set on MSYS2, and every redistributed file must resolve to
# an installed package with a licence payload.
$RuntimePackageFiles = @{}

function Add-MsysPackageAttribution([string]$Package, [string]$PackageRelativePath) {
    if (-not $RuntimePackageFiles.ContainsKey($Package)) {
        $packageLines = @(& $Pacman -Q $Package)
        if ($packageLines.Count -ne 1 -or -not $packageLines[0].Trim()) {
            throw "Required MSYS2 runtime package '$Package' is not installed."
        }
        $RuntimePackageFiles[$Package] = [System.Collections.Generic.List[string]]::new()
    }
    $RuntimePackageFiles[$Package].Add($PackageRelativePath.Replace('\', '/'))
}

function Add-MsysRuntimeFile([string]$SourcePath, [string]$PackageRelativePath) {
    $sourceMsysPath = Convert-ToMsysPath $SourcePath
    $ownerLines = @(& $Pacman -Qqo -- $sourceMsysPath)
    if ($ownerLines.Count -ne 1 -or -not $ownerLines[0].Trim()) {
        throw "Could not identify the MSYS2 package that owns redistributed file '$SourcePath'."
    }
    $owner = $ownerLines[0].Trim()
    Add-MsysPackageAttribution $owner $PackageRelativePath
}

# A builder that already has an MSYS2 inih package may cause CMake to select it
# instead of the pinned source fallback. It can be linked statically, so it will
# not necessarily appear in the PE import walk. Attribute it through its pkg-config
# file and still collect the owning package's exact licence.
if (-not (Test-Path -LiteralPath $InihLicensePath -PathType Leaf)) {
    $inihPcDirs = @(Invoke-MingwCapture 'pkg-config --variable=pcfiledir inih')
    if ($inihPcDirs.Count -ne 1 -or -not $inihPcDirs[0].Trim()) {
        throw 'The build did not expose either the pinned inih source licence or an MSYS2 inih package.'
    }
    $inihPcMsys = $inihPcDirs[0].Trim().TrimEnd('/') + '/inih.pc'
    $inihPcWindows = (& $Cygpath -w $inihPcMsys).Trim()
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $inihPcWindows -PathType Leaf)) {
        throw "Unable to resolve the inih pkg-config file '$inihPcMsys'."
    }
    Add-MsysRuntimeFile $inihPcWindows 'moonlight-tv.exe (linked dependency: inih)'
}

# Fontconfig is not just a DLL: its rules describe the Windows system/user font
# directories and family aliases. Bundle those rules so a downloaded package does
# not accidentally depend on the builder's C:\msys64 tree still existing.
$FontConfigSource = (Resolve-Path -LiteralPath (Join-Path $MingwBin '..\etc\fonts')).Path
$FontConfigDestination = Join-Path $PackageDir 'etc\fonts'
if (-not (Test-Path (Join-Path $FontConfigSource 'fonts.conf'))) {
    throw "Fontconfig runtime data was not found at '$FontConfigSource'."
}
Get-ChildItem -LiteralPath $FontConfigSource -File -Recurse | ForEach-Object {
    $relative = $_.FullName.Substring($FontConfigSource.Length + 1)
    Add-MsysRuntimeFile $_.FullName ("etc/fonts/" + $relative)
}
New-Item -ItemType Directory -Force -Path $FontConfigDestination | Out-Null
Get-ChildItem -Force $FontConfigSource | Copy-Item -Recurse -Force -Destination $FontConfigDestination

# The OpenSSL-backed MSYS2 libcurl has a build-machine CA path. The client
# redirects OpenSSL to this exe-relative copy before its HTTPS controller DB
# update, so the portable package does not depend on C:\msys64 existing.
$CaBundleSource = Join-Path $MingwBin '..\etc\ssl\certs\ca-bundle.crt'
$CaBundleDestination = Join-Path $PackageDir 'curl-ca-bundle.crt'
if (-not (Test-Path $CaBundleSource)) {
    throw "The CA bundle from mingw-w64-x86_64-ca-certificates was not found at '$CaBundleSource'."
}
# This file is generated by the ca-certificates post-install hook and therefore
# is not guaranteed to have a pacman file owner even though the package is its
# authoritative input.
Add-MsysPackageAttribution 'mingw-w64-x86_64-ca-certificates' 'curl-ca-bundle.crt'
Copy-Item -Force $CaBundleSource $CaBundleDestination

$Executable = Join-Path $PackageDir 'moonlight-tv.exe'
if (-not (Test-Path $Executable)) {
    throw "CMake install did not produce '$Executable'."
}
$ModulesIniPath = Join-Path $PackageDir 'ss4s_modules.ini'
if (-not (Test-Path $ModulesIniPath)) {
    throw 'CMake install did not produce ss4s_modules.ini.'
}
$ModulesIni = Get-Content -LiteralPath $ModulesIniPath -Raw
$FfmpegModule = [regex]::Match(
    $ModulesIni,
    '(?ms)^\[ffmpeg\][ \t]*\r?\n(?<body>.*?)(?=^\[|\z)'
)
if (-not $FfmpegModule.Success -or
        $FfmpegModule.Groups['body'].Value -notmatch '(?m)^[ \t]*name[ \t]*=[ \t]*D3D11VA \(FFmpeg\)[ \t]*\r?$' -or
        $FfmpegModule.Groups['body'].Value -notmatch '(?m)^[ \t]*video[ \t]*=[ \t]*true[ \t]*\r?$') {
    throw 'ss4s_modules.ini does not declare the required D3D11VA (FFmpeg) Windows video module.'
}

$Objdump = Join-Path $MingwBin 'objdump.exe'
if (-not (Test-Path $Objdump)) {
    throw "objdump was not found at '$Objdump'."
}

# Fail packaging if the executable silently regresses to a console application
# or loses the Explorer metadata supplied by the Windows resource. Both defects
# are otherwise easy to miss on a developer machine and immediately visible to
# someone launching the downloaded archive.
$PeHeaders = (& $Objdump -p $Executable) -join "`n"
if ($LASTEXITCODE -ne 0) {
    throw "objdump failed while checking '$Executable'."
}
if ($PeHeaders -notmatch '(?m)^\s*Subsystem\s+[0-9A-Fa-f]+\s+\(Windows GUI\)\s*$') {
    throw 'moonlight-tv.exe is not a native Windows GUI executable.'
}

$VersionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($Executable)
$ExpectedMetadata = @{
    ProductName = 'Eclipse'
    FileDescription = 'Eclipse game streaming client'
    OriginalFilename = 'moonlight-tv.exe'
}
foreach ($property in $ExpectedMetadata.Keys) {
    if ($VersionInfo.$property -ne $ExpectedMetadata[$property]) {
        throw "Invalid executable $property metadata: '$($VersionInfo.$property)'."
    }
}
if ($VersionInfo.FileVersion -notmatch '^\d+\.\d+\.\d+\.\d+$' -or
        $VersionInfo.ProductVersion -ne $VersionInfo.FileVersion) {
    throw "Invalid executable version metadata: file '$($VersionInfo.FileVersion)', product '$($VersionInfo.ProductVersion)'."
}

# Resolve the transitive PE import closure. This avoids both a machine-specific
# PATH dependency and a stale hand-maintained list of FFmpeg/SDL dependencies.
$seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$queue = [System.Collections.Generic.Queue[string]]::new()
Get-ChildItem -File $PackageDir | Where-Object { $_.Extension -in @('.exe', '.dll') } |
    ForEach-Object { $queue.Enqueue($_.FullName) }

while ($queue.Count -gt 0) {
    $binary = $queue.Dequeue()
    $imports = & $Objdump -p $binary |
        Select-String -Pattern '^\s*DLL Name:\s*(\S+)\s*$' |
        ForEach-Object { $_.Matches[0].Groups[1].Value }
    if ($LASTEXITCODE -ne 0) {
        throw "objdump failed for '$binary'."
    }

    foreach ($import in $imports) {
        if (-not $seen.Add($import)) {
            continue
        }

        $packaged = Join-Path $PackageDir $import
        if (Test-Path $packaged) {
            $queue.Enqueue($packaged)
            continue
        }

        $mingwRuntime = Join-Path $MingwBin $import
        if (Test-Path $mingwRuntime) {
            Copy-Item -Force $mingwRuntime $packaged
            Add-MsysRuntimeFile $mingwRuntime $import
            $queue.Enqueue($packaged)
            continue
        }

        # API-set contracts are resolved by the Windows loader and do not exist
        # as redistributable files on every supported Windows release.
        if ($import -like 'api-ms-win-*' -or $import -like 'ext-ms-win-*') {
            continue
        }
        $systemDll = Join-Path (Join-Path $env:WINDIR 'System32') $import
        if (Test-Path $systemDll) {
            continue
        }
        throw "Required DLL '$import' (imported by '$binary') was not found in the package, MinGW64, or Windows."
    }
}

# Full runtime notices are mandatory. The supplement is tied to exact package
# versions and immutable upstream inputs; a dependency update must be audited.
$RuntimeLicenseRoot = Join-Path $LicenseDir 'msys2'
New-Item -ItemType Directory -Force -Path $RuntimeLicenseRoot | Out-Null
$SupplementRoot = Join-Path $ProjectRoot 'docs\licenses\windows\msys2'
$Supplement = Get-Content -Raw -LiteralPath (Join-Path $SupplementRoot 'manifest.json') | ConvertFrom-Json
if ($Supplement.schema -ne 1) { throw 'Unsupported runtime notice manifest.' }
$SupplementByPackage = @{}
foreach ($entry in $Supplement.packages) {
    if ($SupplementByPackage.ContainsKey($entry.package)) { throw "Duplicate runtime notice package: $($entry.package)" }
    $SupplementByPackage[$entry.package] = $entry
    foreach ($file in @($entry.notices) + @($entry.recipe_files)) {
        if ($file.path -match '(^/|\\|(^|/)\.\.(/|$)|:)') { throw "Invalid notice path: $($file.path)" }
        $source = Join-Path $SupplementRoot $file.path
        if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $file.sha256) {
            throw "Runtime notice/provenance hash mismatch: $($file.path)"
        }
    }
}
Copy-Item -LiteralPath (Join-Path $SupplementRoot 'manifest.json') -Destination (Join-Path $RuntimeLicenseRoot 'supplement-manifest.json')
Copy-Item -LiteralPath (Join-Path $SupplementRoot 'README.md') -Destination (Join-Path $RuntimeLicenseRoot 'supplement-README.md')
Copy-Item -LiteralPath (Join-Path $SupplementRoot 'provenance') -Destination $RuntimeLicenseRoot -Recurse
$RuntimeLicenseRecords = @()
foreach ($package in @($RuntimePackageFiles.Keys) | Sort-Object) {
    $versionLines = @(& $Pacman -Q $package)
    if ($versionLines.Count -ne 1 -or -not $versionLines[0].Trim()) {
        throw "Unable to record the installed version of MSYS2 package '$package'."
    }

    $packageMetadata = @(& $Pacman -Qi $package)
    if ($packageMetadata.Count -eq 0) {
        throw "Unable to record metadata for MSYS2 package '$package'."
    }
    $safePackageName = $package -replace '[^A-Za-z0-9._-]', '_'
    $packageLicenseDir = Join-Path $RuntimeLicenseRoot $safePackageName
    New-Item -ItemType Directory -Force -Path $packageLicenseDir | Out-Null
    $metadataDestination = Join-Path $packageLicenseDir 'PACKAGE-METADATA.txt'
    @(
        'Metadata reported by the installed, signature-validated MSYS2 package.'
        'Full installed or source-verified supplemental notices are included beside this metadata.'
        ''
        $packageMetadata
    ) | Set-Content -Encoding utf8 $metadataDestination

    $packageListing = @(& $Pacman -Ql $package)
    $licenseMsysPaths = @($packageListing | ForEach-Object {
        if ($_ -match '^\S+\s+(.+)$') { $Matches[1] }
    } | Where-Object {
        $_ -match '/share/licenses/.+[^/]$'
    } | Sort-Object -Unique)
    $copiedLicenses = @($metadataDestination.Substring($PackageDir.Length + 1).Replace('\', '/'))
    foreach ($licenseMsysPath in $licenseMsysPaths) {
        $licenseWindowsPath = (& $Cygpath -w $licenseMsysPath).Trim()
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $licenseWindowsPath -PathType Leaf)) {
            throw "MSYS2 package '$package' lists a missing licence file '$licenseMsysPath'."
        }
        $marker = '/share/licenses/'
        $markerIndex = $licenseMsysPath.IndexOf($marker, [System.StringComparison]::Ordinal)
        $licenseTail = $licenseMsysPath.Substring($markerIndex + $marker.Length).Replace('/', '\')
        $licenseDestination = Join-Path $packageLicenseDir $licenseTail
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $licenseDestination) | Out-Null
        Copy-Item -LiteralPath $licenseWindowsPath -Destination $licenseDestination -Force
        $copiedLicenses += $licenseDestination.Substring($PackageDir.Length + 1).Replace('\', '/')
    }

    if ($SupplementByPackage.ContainsKey($package)) {
        $entry = $SupplementByPackage[$package]
        if ($versionLines[0].Trim() -ne "$package $($entry.version)") {
            throw "Runtime notice version mismatch for '$package': installed $($versionLines[0]); audited $($entry.version)."
        }
        foreach ($notice in $entry.notices) {
            $prefix = "$package/"
            if (-not $notice.path.StartsWith($prefix, [StringComparison]::Ordinal)) {
                throw "Notice does not belong to '$package': $($notice.path)"
            }
            $destination = Join-Path $packageLicenseDir $notice.path.Substring($prefix.Length)
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
            Copy-Item -LiteralPath (Join-Path $SupplementRoot $notice.path) -Destination $destination -Force
            $copiedLicenses += $destination.Substring($PackageDir.Length + 1).Replace('\', '/')
        }
    }
    if ($copiedLicenses.Count -le 1) {
        throw "Runtime package '$package' has no full licence notices; metadata alone cannot be released."
    }

    $RuntimeLicenseRecords += [PSCustomObject]@{
        Package = $versionLines[0].Trim()
        Files = @($RuntimePackageFiles[$package] | Sort-Object -Unique)
        Licenses = @($copiedLicenses | Sort-Object -Unique)
    }
}

$NoticeLines = @(
    'Eclipse Windows portable package - third-party notices'
    ''
    'This file is generated from the exact source inputs and MSYS2 runtime files in this package.'
    'Full licence notices and exact MSYS2 package metadata are under licenses/. Source-verified supplements cover packages that omit installed notices.'
    'Eclipse itself is distributed under GPL-3.0; see LICENSE.txt.'
    ''
    'SOURCE COMPONENTS COMPILED INTO THE CLIENT'
)
foreach ($spec in $SourceLicenseSpecs) {
    $NoticeLines += "- $($spec.Name)"
    $NoticeLines += "  Source: $($spec.Source)"
    $NoticeLines += "  Licence: licenses/source/$($spec.File)"
}
$NoticeLines += @(
    ''
    'DERIVATIVE AND EMBEDDED-SOURCE NOTICES'
    '- Moonlight Qt-derived SS4S desktop video module'
    '  Upstream revision: 032529d782242e3833e0b3b147dbbf96e878e3ca'
    '  Modification notice: licenses/source/Moonlight-Qt-Derivative-NOTICE.md'
    '  Licence: licenses/source/Moonlight-Qt-GPL-3.0.txt'
    '- Moonlight Common C Reed-Solomon implementation'
    '  Notice: licenses/source/Moonlight-Common-C-Reed-Solomon-BSD.txt'
    ''
    'EXTERNAL DATA AND EMBEDDED ASSETS'
    "- SDL_GameControllerDB at revision $ControllerDbRevision"
    '  Source: https://github.com/gabomdq/SDL_GameControllerDB'
    '  Licence: licenses/external/SDL_GameControllerDB-LICENSE.txt'
    "- Google Material Icons (embedded font; licence pinned at revision $MaterialIconsRevision)"
    '  Source: https://github.com/google/material-design-icons'
    '  Licence: licenses/external/Material-Icons-Apache-2.0.txt'
    '- Montserrat 7.200 glyph data embedded through LVGL'
    '  Licence: licenses/external/Montserrat-7.200-OFL-1.1.txt'
    '- Font Awesome Free 5.9.0 glyph data embedded through LVGL'
    '  Licence summary and brand warning: licenses/external/Font-Awesome-Free-5.9.0-LICENSE.txt'
    '  Complete font terms: licenses/external/SIL-OFL-1.1.txt'
    '- Unscii-8 1.0 glyph data embedded through LVGL'
    '  Provenance and upstream terms: licenses/external/Unscii-8-1.0-PROVENANCE.txt'
    '  Exact generated-array and source-font hashes: licenses/external/LVGL-Embedded-Fonts-NOTICE.md'
    ''
    'REDISTRIBUTED MSYS2 RUNTIME PACKAGES'
)
foreach ($record in $RuntimeLicenseRecords) {
    $NoticeLines += "- $($record.Package)"
    $NoticeLines += '  Package files:'
    foreach ($file in $record.Files) { $NoticeLines += "    $file" }
    $NoticeLines += '  Licence copies:'
    foreach ($license in $record.Licenses) { $NoticeLines += "    $license" }
}
$NoticeLines | Set-Content -Encoding utf8 (Join-Path $PackageDir 'THIRD-PARTY-NOTICES.txt')

# Reject checkout/user paths in every shipped PE file. Prefix-map flags above
# cover compiler-provided paths; this assertion also catches generated sources,
# resource strings, or a future dependency target that drops those flags.
$Strings = Join-Path $MingwBin 'strings.exe'
if (-not (Test-Path $Strings)) {
    throw "strings.exe was not found at '$Strings'."
}
$SensitivePathSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($path in @($ProjectRoot, $BuildDir, $SourceMsysPath, $BuildMsysPath)) {
    if ($path) {
        [void] $SensitivePathSet.Add($path)
        [void] $SensitivePathSet.Add($path.Replace('\', '/'))
    }
}
# The builder's PROFILE is checked only in binaries this build produced. MSYS2 runtime
# DLLs are prebuilt by MSYS2's own CI on GitHub-hosted runners, whose profile is
# C:\Users\runneradmin -- the same as ours on a GitHub runner -- and its Rust packages
# keep those paths (librav1e.dll, librsvg-2-2.dll carry their cargo registry paths). That
# is the package builder's machine, not this one, and it failed every CI run.
$ProfilePathSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
if ($env:USERPROFILE) {
    [void] $ProfilePathSet.Add($env:USERPROFILE)
    [void] $ProfilePathSet.Add($env:USERPROFILE.Replace('\', '/'))
    $profileLeaf = Split-Path -Leaf $env:USERPROFILE
    if ($profileLeaf) {
        [void] $ProfilePathSet.Add("/Users/$profileLeaf/")
        [void] $ProfilePathSet.Add("\Users\$profileLeaf\")
    }
}
$RedistributedFiles = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($packageFiles in $RuntimePackageFiles.Values) {
    foreach ($packageFile in $packageFiles) { [void] $RedistributedFiles.Add($packageFile) }
}
foreach ($binary in (Get-ChildItem -LiteralPath $PackageDir -File |
        Where-Object { $_.Extension -in @('.exe', '.dll') })) {
    $binaryPaths = @($SensitivePathSet)
    if (-not $RedistributedFiles.Contains($binary.Name)) {
        $binaryPaths += @($ProfilePathSet)
    }
    foreach ($stringsArguments in @(@('-a'), @('-a', '--encoding=l'))) {
        $binaryStrings = @(& $Strings @stringsArguments $binary.FullName)
        if ($LASTEXITCODE -ne 0) {
            throw "strings.exe failed while auditing '$($binary.FullName)'."
        }
        foreach ($sensitivePath in $binaryPaths) {
            if ($binaryStrings | Select-String -Pattern $sensitivePath -SimpleMatch -Quiet) {
                throw "Builder path '$sensitivePath' is embedded in packaged binary '$($binary.Name)'."
            }
        }
    }
}

$SourceState = if ($SourceWasDirty) {
    "$Commit (modified before dependency bootstrap; NOT release-reproducible)"
} else {
    "$Commit (clean before dependency bootstrap)"
}
$CmakeVersion = (& (Join-Path $MingwBin 'cmake.exe') --version | Select-Object -First 1)
$CompilerVersion = (& (Join-Path $MingwBin 'gcc.exe') --version | Select-Object -First 1)
$PackageVersions = foreach ($package in $Packages) {
    $installedVersion = & $Pacman -Q $package 2>$null
    if ($LASTEXITCODE -eq 0) {
        $installedVersion.Trim()
    } else {
        # -SkipPackageInstall is useful for an already-provisioned builder and
        # may intentionally omit a declared package.  Record that fact without
        # emitting a misleading pacman error or dropping the row entirely.
        "$package (not installed)"
    }
}
$PackageVersions = $PackageVersions | Sort-Object

$DependencyPaths = @(
    'core/moonlight-common-c',
    'core/moonlight-common-c/enet',
    'third_party/commons',
    'third_party/libmicrodns',
    'third_party/lvgl',
    'third_party/ss4s',
    'third_party/Unity'
)
$DependencyState = @()
foreach ($dependency in $DependencyPaths) {
    $dependencyPath = Join-Path $ProjectRoot $dependency
    if (-not (Test-Path (Join-Path $dependencyPath '.git'))) {
        throw "Required client dependency '$dependency' is not initialised."
    }
    $dependencyHead = (& $Git.Source -C $dependencyPath rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to resolve the Git revision of '$dependency'."
    }
    $dependencyDirty = @(& $Git.Source -C $dependencyPath status --porcelain=v1 --untracked-files=all)
    $dependencyMsys = Quote-Bash (Convert-ToMsysPath $dependencyPath)
    $diffHashLines = @(Invoke-MingwCapture "cd $dependencyMsys && git diff --binary --no-ext-diff HEAD | git hash-object --stdin")
    if ($diffHashLines.Count -ne 1 -or -not $diffHashLines[0].Trim()) {
        throw "Unable to hash the applied source delta for '$dependency'."
    }
    $DependencyState += "$dependency $dependencyHead diff=$($diffHashLines[0].Trim()) status_entries=$($dependencyDirty.Count)"
}

$PatchState = @(Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'patches\client-deps') -File |
    Sort-Object Name | ForEach-Object {
        $hash = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant()
        "patches/client-deps/$($_.Name) sha256=$hash"
    })
$BootstrapHash = (Get-FileHash -Algorithm SHA256 (Join-Path $ProjectRoot 'scripts\bootstrap_client_deps.sh')).Hash.ToLowerInvariant()

@(
    'Eclipse Windows source manifest'
    "Root source: $SourceState"
    "Root dirty entries before bootstrap: $($InitialDirtyEntries.Count)"
    "Bootstrap script: scripts/bootstrap_client_deps.sh sha256=$BootstrapHash"
    ''
    'Resolved client dependencies (diff is Git blob hash of the applied worktree delta):'
    $DependencyState
    ''
    'Repository-owned dependency patches:'
    $PatchState
) | Set-Content -Encoding utf8 (Join-Path $PackageDir 'SOURCE-MANIFEST.txt')

@(
    'THIS WINDOWS BUILD IS NOT CODE-SIGNED.'
    ''
    'Windows SmartScreen may warn when you launch moonlight-tv.exe.'
    'Verify SHA256SUMS (and the adjacent archive .sha256 file when downloaded) before running it.'
    'Do not grant Administrator privileges to the client; they are not required.'
) | Set-Content -Encoding ascii (Join-Path $PackageDir 'UNSIGNED-BUILD.txt')

$BuildInfoContractLines = @(
    'Code signing: NONE (unsigned community build)'
    'Hardware video decode: physical-GPU D3D11VA REQUIRED for Windows functional tests'
    'Software decoder override: ECLIPSE_DIAGNOSTIC_VIDEO_DECODER=software is diagnostics only; not a valid functional test'
    'Presentation path: D3D11VA decode with GPU-to-CPU readback and I420 conversion (not zero-copy)'
    'Codec/adapter preflight: H.264/HEVC D3D11 decoder profile and required NV12 output format'
    'Trial decode: no embedded trial bitstream; first live hardware frame remains fail-closed validation'
)
$BuildInfoPath = Join-Path $PackageDir 'BUILDINFO.txt'
@(
    'Eclipse Windows portable build'
    "Version: $($VersionInfo.ProductVersion)"
    "Source: $SourceState"
    "Configuration: $Configuration"
    $BuildInfoContractLines
    "Controller database revision: $ControllerDbRevision"
    "Controller database SHA-256: $ControllerDbSha256"
    "Controller database licence SHA-256: $ControllerDbLicenseSha256"
    'TLS roots: mingw-w64-x86_64-ca-certificates (curl-ca-bundle.crt)'
    $CmakeVersion
    $CompilerVersion
    ''
    'Declared MSYS2 packages:'
    $PackageVersions
) | Set-Content -Encoding utf8 $BuildInfoPath

$RecordedBuildInfo = @(Get-Content -LiteralPath $BuildInfoPath)
foreach ($requiredLine in $BuildInfoContractLines) {
    if ($RecordedBuildInfo -cnotcontains $requiredLine) {
        throw "BUILDINFO.txt does not record required release fact: $requiredLine"
    }
}

Copy-Item -LiteralPath (Join-Path $ProjectRoot 'deploy\windows\README.txt') -Destination (Join-Path $PackageDir 'README.txt')

$Hashes = Get-ChildItem -File -Recurse $PackageDir | Sort-Object FullName | ForEach-Object {
    $hash = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant()
    # Windows PowerShell 5.1 targets .NET Framework, which has no
    # System.IO.Path.GetRelativePath(). Both paths are already absolute.
    $relative = $_.FullName.Substring($PackageDir.Length + 1).Replace('\', '/')
    "$hash  $relative"
}
$Hashes | Set-Content -Encoding ascii (Join-Path $PackageDir 'SHA256SUMS')

if (-not $NoArchive) {
    Compress-Archive -Path $PackageDir -DestinationPath $Archive -CompressionLevel Optimal
    $archiveDigest = (Get-FileHash -Algorithm SHA256 $Archive).Hash.ToLowerInvariant()
    "$archiveDigest  $([System.IO.Path]::GetFileName($Archive))" |
        Set-Content -Encoding ascii $ArchiveHash
    Write-Host "Archive: $Archive"
    Write-Host "Archive SHA-256: $ArchiveHash"
}
Write-Host "Portable package: $PackageDir"
