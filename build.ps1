$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $projectRoot 'src\main.cpp'
$buildDir = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

$gxx = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory |
  Where-Object { $_.Name -like 'BrechtSanders.WinLibs.MCF.UCRT*' } |
  ForEach-Object { Join-Path $_.FullName 'mingw64\bin\g++.exe' } |
  Where-Object { Test-Path $_ } |
  Select-Object -First 1

if (-not $gxx) {
  throw 'g++.exe not found. Please install BrechtSanders.WinLibs.MCF.UCRT via winget.'
}

$outExe = Join-Path $buildDir 'shubiaoguiji.exe'

& $gxx `
  -std=c++20 `
  -O2 `
  -DNOMINMAX `
  -DWIN32_LEAN_AND_MEAN `
  -municode `
  -mwindows `
  $src `
  -o $outExe `
  -lgdiplus -lcomdlg32 -lshell32 -lgdi32 -lole32 -luuid

if ($LASTEXITCODE -ne 0) {
  throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host "Built: $outExe"
