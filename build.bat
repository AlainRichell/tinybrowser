@echo off
rem Builds tinyweb.exe: a single statically linked binary, no runtime to ship.
setlocal enabledelayedexpansion
set "ROOT=%~dp0"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  set "VS="
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VS=%%i"
  if not defined VS (
    echo No encontre el toolset de C++. Instala Visual Studio Build Tools 2022 con la carga "Desarrollo de escritorio con C++".
    exit /b 1
  )
  set "VCVARS=!VS!\VC\Auxiliary\Build\vcvars64.bat"
)

call "%VCVARS%" >nul
if errorlevel 1 (
  echo No pude inicializar el entorno de Visual Studio.
  exit /b 1
)

if not exist "%ROOT%obj" mkdir "%ROOT%obj"

if not exist "%ROOT%sdk\x64\WebView2LoaderStatic.lib" (
  echo El SDK de WebView2 no esta: lo descargo.
  powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\fetch-sdk.ps1" "%~dp0."
  if errorlevel 1 (
    echo No pude descargar el SDK de WebView2.
    exit /b 1
  )
)

rc /nologo /fo"%ROOT%obj\tinybrowser.res" "%ROOT%tinybrowser.rc"
if errorlevel 1 (
  echo Fallo la compilacion del recurso del icono.
  exit /b 1
)

cl /nologo /O2 /MT /EHsc /W4 /I"%ROOT%sdk\include" ^
   "%ROOT%tinybrowser.cpp" /Fe:"%ROOT%tinybrowser.exe" /Fo:"%ROOT%obj\\" ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO ^
   "%ROOT%obj\tinybrowser.res" ^
   "%ROOT%sdk\x64\WebView2LoaderStatic.lib" ^
   ole32.lib oleaut32.lib user32.lib gdi32.lib shell32.lib shlwapi.lib version.lib advapi32.lib ^
   d3d11.lib dcomp.lib
if errorlevel 1 (
  echo Fallo la compilacion.
  exit /b 1
)

for %%f in ("%ROOT%tinybrowser.exe") do echo Listo: %%~ff  (%%~zf bytes)
