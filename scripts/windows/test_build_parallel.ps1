# Parses the builder and executes only its bounded parallel-argument selection block.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$path = Join-Path $PSScriptRoot 'build_portable.ps1'
$source = [IO.File]::ReadAllText($path)
$tokens = $null; $errors = $null
[Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$errors) | Out-Null
if ($errors.Count) { throw "Builder syntax errors: $errors" }
$match = [regex]::Match($source, '(?ms)^\$BuildParallelArguments = .*?(?=^\$ProjectRoot = )')
if (-not $match.Success) { throw 'Missing actual builder parallel block' }
$block = [scriptblock]::Create($match.Value + "`nreturn `$BuildParallelArguments")
$original = $env:CMAKE_BUILD_PARALLEL_LEVEL
try {
    foreach ($value in @($null, '', '1', '2', '16', '0', '-2', '2.5', ' 2', '2 ', '+2', '02', 'two', '2147483648', '2; echo unwanted')) {
        $env:CMAKE_BUILD_PARALLEL_LEVEL = $value
        $valid = $null -eq $value -or $value -eq '' -or $value -in @('1', '2', '16')
        $failed = $false; $actual = $null
        try { $actual = & $block } catch { $failed = $true }
        if ($valid) {
            $expected = if ($value) { '--parallel ' + $value } else { '--parallel' }
            if ($failed -or $actual -ne $expected) { throw "Unexpected argument selection for '$value': $actual" }
        } elseif (-not $failed) { throw "Invalid value accepted: $value" }
    }
} finally { $env:CMAKE_BUILD_PARALLEL_LEVEL = $original }
Write-Host 'PASS actual Windows builder selection: unset/empty default; positive explicit cap; malformed/overflow/injection refused'
Write-Host 'No compiler, package manager, application or installer executed.'
