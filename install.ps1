# Installs Ixo from a GitHub release (Windows).
#
#   irm https://raw.githubusercontent.com/Eqixo/Ixo/main/install.ps1 | iex
#
# Settings (environment variables):
#   IXO_VERSION=v1.0.0     a specific release (default: the latest)
#   IXO_HOME               where it goes (default: %LOCALAPPDATA%\Ixo; the binary lands in bin\)
#   IXO_ARCHIVE=file.zip   install from a downloaded archive instead
#   IXO_NO_PATH=1          don't add it to the user PATH
$ErrorActionPreference = "Stop"

$repo = "Eqixo/Ixo"
$name = "ixo-windows-x86_64"
$home_ = if ($env:IXO_HOME) { $env:IXO_HOME } else { Join-Path $env:LOCALAPPDATA "Ixo" }
$bin = Join-Path $home_ "bin"

if (-not [Environment]::Is64BitOperatingSystem) { throw "ixo install: only 64-bit Windows is supported" }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("ixo-install-" + [Guid]::NewGuid())
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
    $zip = Join-Path $tmp "$name.zip"
    if ($env:IXO_ARCHIVE) {
        Copy-Item $env:IXO_ARCHIVE $zip
    } else {
        $url = if ($env:IXO_VERSION) { "https://github.com/$repo/releases/download/$($env:IXO_VERSION)/$name.zip" }
               else { "https://github.com/$repo/releases/latest/download/$name.zip" }
        Write-Host "downloading $url"
        $ProgressPreference = "SilentlyContinue"   # much faster downloads in Windows PowerShell
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    }
    Expand-Archive -Path $zip -DestinationPath $tmp -Force
    $src = Join-Path $tmp $name
    if (-not (Test-Path (Join-Path $src "ixo.exe"))) { throw "ixo install: the archive has no $name\ixo.exe" }

    New-Item -ItemType Directory -Force $bin | Out-Null
    foreach ($d in @("docs", "examples")) {
        $target = Join-Path $home_ $d
        if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    }
    Copy-Item (Join-Path $src "ixo.exe") (Join-Path $bin "ixo.exe") -Force
    foreach ($extra in @("docs", "examples", "README.md")) {
        $p = Join-Path $src $extra
        if (Test-Path $p) { Copy-Item $p $home_ -Recurse -Force }
    }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

$version = & (Join-Path $bin "ixo.exe") version
Write-Host "installed $version to $bin\ixo.exe"

if (-not $env:IXO_NO_PATH) {
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $parts = if ($userPath) { $userPath.Split(";") } else { @() }
    if ($parts -notcontains $bin) {
        [Environment]::SetEnvironmentVariable("Path", (($parts + $bin) | Where-Object { $_ }) -join ";", "User")
        Write-Host "added $bin to your PATH (open a new terminal to use it)"
    }
}
