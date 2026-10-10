# Native Git checkout fixture only. Never builds or applies patches to real dependencies.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$git=(Get-Command git -ErrorAction Stop).Source
$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$attributes=[IO.File]::ReadAllText((Join-Path $root '.gitattributes'))
$patch=[IO.File]::ReadAllBytes((Join-Path $root 'patches/client-deps/ss4s.patch'))
$lf=[Text.Encoding]::UTF8.GetString($patch).Replace("`r`n","`n")
$temp=Join-Path ([IO.Path]::GetTempPath()) ('eclipse-patch-checkout-'+[guid]::NewGuid().ToString('N'))
function GitChecked([string[]]$argsList) {
 & $git @argsList | Out-Null
 if($LASTEXITCODE){throw "Git fixture failed: $argsList"}
}
New-Item -ItemType Directory -Path $temp | Out-Null
try {
 foreach($fixed in @($false,$true)) {
  $repo=Join-Path $temp ([string]$fixed)
  New-Item -ItemType Directory -Path (Join-Path $repo 'patches/client-deps') -Force | Out-Null
  GitChecked @('-C',$repo,'init','--quiet')
  GitChecked @('-C',$repo,'config','core.autocrlf','true')
  GitChecked @('-C',$repo,'config','user.name','Isolated checkout fixture')
  GitChecked @('-C',$repo,'config','user.email','fixture@example.invalid')
  $attr=if($fixed){$attributes}else{[regex]::Replace($attributes,'(?m)^patches/client-deps/\*\.patch text eol=lf\r?\n?','')}
  [IO.File]::WriteAllText((Join-Path $repo '.gitattributes'),$attr,[Text.UTF8Encoding]::new($false))
  $file=Join-Path $repo 'patches/client-deps/ss4s.patch'
  [IO.File]::WriteAllText($file,$lf,[Text.UTF8Encoding]::new($false))
  GitChecked @('-C',$repo,'add','.')
  GitChecked @('-C',$repo,'commit','--quiet','-m','Synthetic exact patch checkout')
  [IO.File]::Delete($file)
  GitChecked @('-C',$repo,'checkout','--','patches/client-deps/ss4s.patch')
  $actual=[IO.File]::ReadAllText($file)
  if($fixed) {
   if($actual -cne $lf -or $actual.Contains("`r")){throw 'Fixed checkout is not exact LF patch bytes'}
  } elseif(-not $actual.Contains("`r`n")){throw 'Baseline did not reproduce native CRLF conversion'}
  $dirty=@(& $git -C $repo status --porcelain=v1)
  if($LASTEXITCODE -or $dirty.Count){throw 'Fixture checkout is dirty'}
  Write-Host "PASS native Git autocrlf=true fixed=$fixed; exact expected checkout bytes, clean state"
 }
} finally {
 Remove-Item -LiteralPath $temp -Recurse -Force
 if(Test-Path -LiteralPath $temp){throw 'Owned fixture residue'}
}
Write-Host 'PASS owned fixture directories removed; no real dependency or application touched'
