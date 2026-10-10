Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=Join-Path $env:TEMP ('eclipse installer smoke '+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $root | Out-Null
$owned=Join-Path $root 'owned'; $unrelated=Join-Path $root 'unrelated'
New-Item -ItemType Directory $owned,$unrelated | Out-Null
$fixture=@'
using System;
using System.Threading;
public class Fixture { public static void Main() { Thread.Sleep(120000); } }
'@
Add-Type -TypeDefinition $fixture -OutputType ConsoleApplication -OutputAssembly (Join-Path $owned 'moonlight-tv.exe')
Copy-Item (Join-Path $owned 'moonlight-tv.exe') (Join-Path $unrelated 'moonlight-tv.exe')
@{product='Eclipse-client';schema=1;files=@('moonlight-tv.exe','owned.txt')} | ConvertTo-Json | Set-Content (Join-Path $owned 'eclipse-install.json')
Set-Content (Join-Path $owned 'owned.txt') 'owned'
Set-Content (Join-Path $owned 'user-created.txt') 'preserve'
$ours=$null; $other=$null
try {
    $ours=Start-Process (Join-Path $owned 'moonlight-tv.exe') -WindowStyle Hidden -PassThru
    $other=Start-Process (Join-Path $unrelated 'moonlight-tv.exe') -WindowStyle Hidden -PassThru
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot '..\..\deploy\windows\manage-install.ps1') -Action Remove -InstallDir $owned -KeepSettings
    if($LASTEXITCODE -ne 0){throw 'cleanup helper failed'}
    $ours.Refresh(); $other.Refresh()
    if(-not $ours.HasExited){throw 'owned process survived'}
    if($other.HasExited){throw 'unrelated process was killed'}
    if(Test-Path (Join-Path $owned 'moonlight-tv.exe')){throw 'owned exe remains'}
    if(Test-Path (Join-Path $owned 'eclipse-install.json')){throw 'marker remains'}
    if(-not(Test-Path (Join-Path $owned 'user-created.txt'))){throw 'unowned data was removed'}
    Write-Output 'PASS: bounded exact-path process cleanup, owned file removal, unrelated process and file retained'
    $junction=Join-Path $root 'junction'
    New-Item -ItemType Junction -Path $junction -Target $unrelated | Out-Null
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot '..\..\deploy\windows\manage-install.ps1') -Action Prepare -InstallDir $junction
    if($LASTEXITCODE -eq 0){throw 'junction accepted'}
    (Get-Item -LiteralPath $junction).Delete()
    Write-Output 'PASS: junction target refused before mutation'
} finally {
    foreach($p in @($ours,$other)){if($p){$p.Refresh();if(-not $p.HasExited){$p.Kill();[void]$p.WaitForExit(5000)}}}
    Remove-Item -LiteralPath $root -Recurse -Force
}
