@echo off
rem Builds build\oniria.exe with MSVC. Fetches raylib on first run.
setlocal
set "ROOT=%~dp0"

if exist "%ROOT%third_party\raylib\include\raylib.h" goto have_raylib
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\fetch_raylib.ps1"
if errorlevel 1 exit /b 1
:have_raylib

where cl >nul 2>nul
if not errorlevel 1 goto have_cl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_msvc
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT goto no_msvc
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 goto no_msvc
:have_cl

if not exist "%ROOT%build" mkdir "%ROOT%build"
cl /nologo /std:c++17 /O2 /fp:fast /EHsc /utf-8 /W3 /MD /DNDEBUG ^
   /I"%ROOT%third_party\raylib\include" ^
   /Fo"%ROOT%build\\" /Fe"%ROOT%build\oniria.exe" ^
   "%ROOT%src\main.cpp" "%ROOT%src\analysis.cpp" "%ROOT%src\sfx.cpp" "%ROOT%src\platform_win.cpp" ^
   /link /LIBPATH:"%ROOT%third_party\raylib\lib" ^
   raylib.lib winmm.lib gdi32.lib user32.lib shell32.lib opengl32.lib comdlg32.lib ^
   /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
if errorlevel 1 exit /b 1
echo Built %ROOT%build\oniria.exe
exit /b 0

:no_msvc
echo Could not find the MSVC C++ build tools. Install "Desktop development with C++" from the Visual Studio Installer.
exit /b 1
