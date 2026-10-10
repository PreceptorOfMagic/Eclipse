<# Turn a portable package whose own binaries came back signed from SignPath into the signed
   release package: check every signature, record the signing in the package, re-hash it, and
   write eclipse-windows-x64.zip. Run after build_portable.ps1 -NoArchive and after the signed
   files have been copied over the unsigned ones; build_installer.ps1 then packages the result.

   Only binaries built from this repository are signed (SignPath Foundation's rule: third-party
   DLLs stay as their maintainers shipped them), so -SignedFiles lists our own executables. #>
[CmdletBinding()]
param(
    [string]$PackageDir = (Join-Path $PSScriptRoot '..\..\dist\eclipse-windows-x64'),
    [string[]]$SignedFiles = @('moonlight-tv.exe'),
    # The certificate SignPath Foundation issues to open-source projects. Matched on O=, which
    # survives a certificate renewal; CN and O are both "SignPath Foundation" today.
    [string]$ExpectedOrganization = 'SignPath Foundation',
    [string]$Archive = (Join-Path $PSScriptRoot '..\..\dist\eclipse-windows-x64.zip'),
    # Tests only: lets the packaging steps run on a build that was never sent for signing.
    [switch]$SkipSignatureCheck
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$PackageDir = (Resolve-Path -LiteralPath $PackageDir).Path
$Archive = [IO.Path]::GetFullPath($Archive)

foreach ($relative in $SignedFiles) {
    $path = Join-Path $PackageDir $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Signed file missing from package: $relative" }
    if ($SkipSignatureCheck) { continue }
    $signature = Get-AuthenticodeSignature -LiteralPath $path
    if ($signature.Status -ne 'Valid') { throw "Signature on $relative is $($signature.Status): $($signature.StatusMessage)" }
    $organization = $signature.SignerCertificate.GetNameInfo([Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
    if ($signature.SignerCertificate.Subject -notmatch ('(^|, )O=' + [regex]::Escape($ExpectedOrganization) + '(,|$)')) {
        throw "Unexpected signer on ${relative}: $($signature.SignerCertificate.Subject)"
    }
    # A signature without a timestamp stops validating when the certificate expires.
    if ($null -eq $signature.TimeStamperCertificate) { throw "Signature on $relative has no timestamp" }
    Write-Host "Signed: $relative by $organization (timestamped by $($signature.TimeStamperCertificate.Subject))"
}

$unsignedNote = Join-Path $PackageDir 'UNSIGNED-BUILD.txt'
if (Test-Path -LiteralPath $unsignedNote) { Remove-Item -LiteralPath $unsignedNote }
@(
    'THIS WINDOWS BUILD IS CODE-SIGNED.'
    ''
    "Signed files: $($SignedFiles -join ', ')"
    'Free code signing provided by SignPath.io, certificate by SignPath Foundation.'
    'Code signing policy: CODE_SIGNING_POLICY.md in the source repository.'
    'Third-party DLLs are shipped as their projects built them and are not re-signed.'
    'Verify SHA256SUMS (and the adjacent archive .sha256 file when downloaded) as well.'
) | Set-Content -Encoding ascii (Join-Path $PackageDir 'SIGNED-BUILD.txt')

$buildInfoPath = Join-Path $PackageDir 'BUILDINFO.txt'
$buildInfo = @(Get-Content -LiteralPath $buildInfoPath)
$unsignedLine = 'Code signing: NONE (unsigned community build)'
if ($buildInfo -cnotcontains $unsignedLine) { throw "BUILDINFO.txt does not carry the unsigned marker; was this package already finalized?" }
$signedLine = "Code signing: Authenticode by $ExpectedOrganization via SignPath.io ($($SignedFiles -join ', '))"
if ($SkipSignatureCheck) { $signedLine = 'Code signing: TEST PACKAGE, signatures not checked' }
$buildInfo | ForEach-Object { if ($_ -ceq $unsignedLine) { $signedLine } else { $_ } } |
    Set-Content -Encoding utf8 $buildInfoPath

# Same format as build_portable.ps1, so build_installer.ps1 verifies it the same way.
$hashes = Get-ChildItem -File -Recurse $PackageDir | Where-Object { $_.Name -ne 'SHA256SUMS' -or $_.DirectoryName -ne $PackageDir } |
    Sort-Object FullName | ForEach-Object {
        $hash = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant()
        $relative = $_.FullName.Substring($PackageDir.Length + 1).Replace('\', '/')
        "$hash  $relative"
    }
$hashes | Set-Content -Encoding ascii (Join-Path $PackageDir 'SHA256SUMS')

if (Test-Path -LiteralPath $Archive) { Remove-Item -LiteralPath $Archive }
Compress-Archive -Path $PackageDir -DestinationPath $Archive -CompressionLevel Optimal
$digest = (Get-FileHash -Algorithm SHA256 $Archive).Hash.ToLowerInvariant()
"$digest  $([IO.Path]::GetFileName($Archive))" | Set-Content -Encoding ascii "$Archive.sha256"
Write-Host "Signed package: $PackageDir"
Write-Host "Archive: $Archive ($digest)"
