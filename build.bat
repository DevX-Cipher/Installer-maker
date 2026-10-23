@echo off
setlocal
set "ROOT=%~dp0"
set "BUILD=%ROOT%build"
set "DIST=%ROOT%dist"

where cl >nul 2>nul
if not errorlevel 1 goto have_cl
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSW%" goto no_vs
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto no_vs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
goto have_cl
:no_vs
echo Visual Studio C++ tools not found.
exit /b 1
:have_cl

if not exist "%BUILD%\stub" mkdir "%BUILD%\stub"
if not exist "%BUILD%\maker" mkdir "%BUILD%\maker"
if not exist "%DIST%" mkdir "%DIST%"

set "CFLAGS=/nologo /O2 /MT /W3 /std:c11 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601"
set "INC=/I"%ROOT%src\common" /I"%ROOT%third_party\lzma""
set "LIBS=shell32.lib ole32.lib uuid.lib advapi32.lib user32.lib gdi32.lib gdiplus.lib comdlg32.lib"

echo [1/2] Building setup stub...
rc /nologo /I"%ROOT%src\stub" /fo"%BUILD%\stub\stub.res" "%ROOT%src\stub\stub.rc" || exit /b 1
cl %CFLAGS% %INC% /I"%ROOT%src\stub" /Fo"%BUILD%\stub\\" /Fe"%BUILD%\stub.exe" ^
  "%ROOT%src\stub\stub.c" "%ROOT%src\stub\install.c" "%ROOT%src\stub\ui.c" "%ROOT%src\stub\uninstall_mode.c" ^
  "%ROOT%src\common\common.c" "%ROOT%src\common\config.c" "%ROOT%src\common\draw.c" "%ROOT%src\common\payload.c" ^
  "%BUILD%\stub\stub.res" /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO %LIBS% || exit /b 1

echo [2/2] Building Installer Maker...
rc /nologo /I"%BUILD%" /I"%ROOT%src\maker" /fo"%BUILD%\maker\maker.res" "%ROOT%src\maker\maker.rc" || exit /b 1
cl %CFLAGS% %INC% /I"%ROOT%src\maker" /Fo"%BUILD%\maker\\" /Fe"%DIST%\installer-maker.exe" ^
  "%ROOT%src\maker\maker.c" "%ROOT%src\maker\packer.c" ^
  "%ROOT%src\common\common.c" "%ROOT%src\common\config.c" "%ROOT%src\common\draw.c" ^
  "%BUILD%\maker\maker.res" /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO %LIBS% || exit /b 1

echo.
echo Done: %DIST%\installer-maker.exe
