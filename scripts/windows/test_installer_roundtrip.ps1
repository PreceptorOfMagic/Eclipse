Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$key='HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\EclipseClient'
$settingsKey='HKCU:\Software\EclipseClient'
$shortcutDir=Join-Path ([Environment]::GetFolderPath('Programs')) 'Eclipse'
if ((Test-Path $key) -or (Test-Path $settingsKey) -or (Test-Path $shortcutDir)) { throw 'Existing installer state; cannot run isolated install test in this account.' }
$dest=Join-Path $env:TEMP ('Eclipse install verify '+[guid]::NewGuid().ToString('N'))
$setup=Join-Path $PSScriptRoot '..\..\dist\eclipse-windows-x64-setup-unsigned.exe'
$profile=Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'com.limelight\moonlight-tv'
function ProfileHashes { @((Get-ChildItem -LiteralPath $profile -File -Filter '*.ini' -ErrorAction SilentlyContinue | Sort-Object FullName | Get-FileHash -Algorithm SHA256).Hash) -join ',' }
$before=ProfileHashes
function Install {
    $p=Start-Process $setup -ArgumentList @('/S',"/D=$dest") -Wait -PassThru
    if($p.ExitCode -ne 0){throw "setup failed $($p.ExitCode)"}
    if(-not(Test-Path (Join-Path $dest 'moonlight-tv.exe'))){throw 'payload missing'}
    $verified = 0
    foreach ($line in Get-Content -LiteralPath (Join-Path $dest 'SHA256SUMS')) {
        if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { throw 'Invalid installed checksum manifest' }
        $expected = $Matches[1]; $relative = $Matches[2]
        if ((Get-FileHash -LiteralPath (Join-Path $dest $relative) -Algorithm SHA256).Hash -ne $expected) {
            throw "Installed payload checksum mismatch: $relative"
        }
        $verified++
    }
    if ($verified -eq 0) { throw 'Installed checksum manifest is empty' }
    Write-Output "PASS: all $verified installed payload file hashes"
    if((Get-ItemProperty $key).InstallLocation -ne $dest){throw 'wrong registry install path'}
    if(-not(Test-Path (Join-Path $shortcutDir 'Eclipse.lnk'))){throw 'shortcut missing'}
}
try {
    Install
    Write-Output 'PASS: silent per-user install, Apps uninstall registration and shortcut'
    Set-Content (Join-Path $dest 'unowned.txt') 'keep'
    $manifestPath=Join-Path $dest 'eclipse-install.json'
    $manifest=Get-Content $manifestPath -Raw | ConvertFrom-Json
    $manifest.files=@($manifest.files)+@('obsolete.dll')
    Set-Content (Join-Path $dest 'obsolete.dll') 'old version'
    $manifest | ConvertTo-Json -Depth 3 | Set-Content $manifestPath
    Install
    if(Test-Path (Join-Path $dest 'obsolete.dll')){throw 'upgrade retained obsolete owned file'}
    if(-not(Test-Path (Join-Path $dest 'unowned.txt'))){throw 'upgrade deleted unknown file'}
    Write-Output 'PASS: upgrade replaces payload, removes obsolete owned file, preserves unknown file'
    $p=Start-Process (Join-Path $dest 'Uninstall.exe') -ArgumentList '/S /KEEPSETTINGS' -Wait -PassThru
    if($p.ExitCode -ne 0){throw "uninstall failed $($p.ExitCode)"}
    $deadline=[DateTime]::UtcNow.AddSeconds(20)
    while((Test-Path (Join-Path $dest 'Uninstall.exe')) -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 100}
    if((Test-Path $key) -or (Test-Path $settingsKey) -or (Test-Path $shortcutDir)){throw 'uninstall registration/shortcut remains'}
    $remaining=@(Get-ChildItem -LiteralPath $dest -Recurse -File)
    if($remaining.Count -ne 1 -or $remaining[0].Name -ne 'unowned.txt'){throw "owned files remain: $($remaining.Name -join ',')"}
    if((ProfileHashes) -ne $before){throw 'KEEPSETTINGS modified existing profile'}
    Write-Output 'PASS: uninstall removes payload, shortcuts, registry; retains only unknown file and unchanged profile'
} finally {
    if(Test-Path (Join-Path $dest 'Uninstall.exe')){Start-Process (Join-Path $dest 'Uninstall.exe') -ArgumentList '/S /KEEPSETTINGS' -Wait}
    if(Test-Path $dest){Remove-Item -LiteralPath $dest -Recurse -Force}
}
