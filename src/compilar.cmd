@echo off
rem Compila Pluma.exe (necesita las Build Tools de Visual Studio con C++). El SDK de WebView2 se baja solo la primera vez.
rem   compilar.cmd            compila ..\Pluma.exe (cierra el Pluma que este en memoria)
rem   compilar.cmd <ruta>     compila ahi sin cerrar nada (para pruebas)
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 (echo No encuentro el compilador de C++ & exit /b 1)

cd /d "%~dp0"
set "WV=sdk\pkg\build\native"
if not exist "%WV%\include\WebView2.h" (
  echo Bajando el SDK de WebView2...
  powershell -NoProfile -Command "New-Item -ItemType Directory -Force sdk | Out-Null; Invoke-WebRequest -UseBasicParsing https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2 -OutFile sdk\wv2.zip; Expand-Archive -Force sdk\wv2.zip sdk\pkg" || exit /b 1
)
set "OUT=..\Pluma.exe"
if not "%~1"=="" set "OUT=%~1"
rem el que este en memoria tendria bloqueado el ejecutable (no al compilar a otra ruta, para pruebas)
if "%~1"=="" if exist ..\Pluma.exe ..\Pluma.exe --salir
if "%~1"=="" ping -n 2 127.0.0.1 >nul
rem PLUMA_VERSION=1.2.3 (release.yml): the release this build is, for its updates; without it, "dev" (never updates)
set "VERDEF="
if defined PLUMA_VERSION set "VERDEF=/DPLUMA_VERSION=\"%PLUMA_VERSION%\""
rc /nologo /fo pluma.res pluma.rc || exit /b 1
cl /nologo %VERDEF% /O2 /MT /EHsc /utf-8 /W3 /std:c++17 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I "%WV%\include" ^
   main.cpp tabs.cpp ui.cpp scripts.cpp extensions.cpp video.cpp zoom.cpp lang.cpp update.cpp settings.cpp bookmarks.cpp import.cpp passwords.cpp sync.cpp monocypher\monocypher.c lite.cpp reader.cpp sound.cpp profiles.cpp passgen.cpp translate.cpp pluma.res /Fe:"%OUT%" ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF "%WV%\x64\WebView2LoaderStatic.lib" ^
   advapi32.lib cabinet.lib comdlg32.lib crypt32.lib d2d1.lib dwmapi.lib dwrite.lib gdi32.lib ole32.lib oleaut32.lib psapi.lib shell32.lib shlwapi.lib ^
   bcrypt.lib urlmon.lib user32.lib uxtheme.lib version.lib windowscodecs.lib winhttp.lib ws2_32.lib || exit /b 1
del *.obj pluma.res 2>nul
echo Hecho: %OUT%
