# Builds Ixo on Windows with g++ (MinGW). Incremental and parallel (PowerShell 7+).
#   ./build.ps1            release build -> build/release/ixo.exe
#   ./build.ps1 debug      debug build   -> build/debug/ixo.exe
#   ./build.ps1 clean
param([string]$Mode = "release")
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if ($Mode -eq "clean") { Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue; "cleaned"; exit 0 }
$flags = @("-std=c++20", "-Wall", "-Wextra", "-Wno-unused-parameter")
switch ($Mode) {
    "release" { $flags += "-O2" }
    "debug"   { $flags += @("-O0", "-g") }
    default   { "usage: ./build.ps1 [release|debug|clean]"; exit 2 }
}
$out = "build/$Mode"
$objDir = "$out/obj"
New-Item -ItemType Directory -Force $objDir | Out-Null
$exe = "$out/ixo.exe"

$newestHeader = (Get-ChildItem src/*.hpp | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime
$todo = @()
foreach ($src in Get-ChildItem src/*.cpp) {
    $obj = Join-Path $objDir ($src.BaseName + ".o")
    if (-not (Test-Path $obj) -or $src.LastWriteTime -gt (Get-Item $obj).LastWriteTime -or $newestHeader -gt (Get-Item $obj).LastWriteTime) {
        $todo += $src.FullName
    }
}

if ($todo.Count -gt 0) {
    "compiling $($todo.Count) file(s) ($Mode)..."
    $results = $todo | ForEach-Object -ThrottleLimit 12 -Parallel {
        $obj = Join-Path $using:objDir ([IO.Path]::GetFileNameWithoutExtension($_) + ".o")
        $msg = & g++ @using:flags -c $_ -o $obj 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { Remove-Item $obj -ErrorAction SilentlyContinue; "FAIL:$msg" } elseif ($msg.Trim()) { $msg }
    }
    $failed = $false
    foreach ($r in $results) {
        if ($r -like "FAIL:*") { $failed = $true; Write-Host $r.Substring(5) } else { Write-Host $r }
    }
    if ($failed) { "build failed"; exit 1 }
}

$objs = Get-ChildItem src/*.cpp | ForEach-Object { Join-Path $objDir ($_.BaseName + ".o") }
$needLink = -not (Test-Path $exe) -or $todo.Count -gt 0
if ($needLink) {
    "linking $exe"
    & g++ @flags -o $exe @objs -static -lwinhttp -lws2_32
    if ($LASTEXITCODE -ne 0) { "link failed"; exit 1 }
}
"built $exe"
