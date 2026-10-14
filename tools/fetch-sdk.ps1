# Fetches the three WebView2 SDK files this project needs, so the repository does not
# carry 14 MB of Microsoft's headers and import libraries. build.bat calls this when the
# SDK is missing; run it by hand only if you want the files without compiling.
param([string]$Root = (Split-Path -Parent $PSScriptRoot))

$ErrorActionPreference = "Stop"
$Root = $Root.TrimEnd('\', '/')   # a trailing backslash inside quotes escapes the quote
$version = "1.0.4258.31"
$lib = Join-Path $Root "sdk\x64\WebView2LoaderStatic.lib"

if (Test-Path $lib) {
    Write-Host "SDK de WebView2 ya presente, no descargo nada."
    exit 0
}

$url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$version/microsoft.web.webview2.$version.nupkg"
$package = Join-Path $env:TEMP "webview2-$version.nupkg"
$archive = Join-Path $env:TEMP "webview2-$version.zip"
$extract = Join-Path $env:TEMP "webview2-$version"

Write-Host "Descargando WebView2 SDK $version (unos 10 MB)..."
Invoke-WebRequest -Uri $url -OutFile $package
Copy-Item $package $archive -Force          # Expand-Archive insists on a .zip name
Remove-Item $extract -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive -Path $archive -DestinationPath $extract -Force

New-Item -ItemType Directory -Force -Path (Join-Path $Root "sdk\include") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Root "sdk\x64") | Out-Null
Copy-Item (Join-Path $extract "build\native\include\WebView2.h") (Join-Path $Root "sdk\include") -Force
Copy-Item (Join-Path $extract "build\native\include\WebView2EnvironmentOptions.h") (Join-Path $Root "sdk\include") -Force
Copy-Item (Join-Path $extract "build\native\x64\WebView2LoaderStatic.lib") (Join-Path $Root "sdk\x64") -Force

Remove-Item $package, $archive -Force -ErrorAction SilentlyContinue
Remove-Item $extract -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "SDK listo en $Root\sdk"
