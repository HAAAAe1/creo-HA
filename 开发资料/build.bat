@echo off
rem =====================================================================
rem  H - Creo TOOLKIT plugin build script (Creo 6.0 Toolkit)
rem  Just double-click this file. It locates Visual Studio by itself.
rem =====================================================================
setlocal

cd /d "%~dp0"

set "VSDIR="
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
if not defined VSDIR if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
if not defined VSDIR if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not defined VSDIR if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Professional"
if not defined VSDIR if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Enterprise"

if not defined VSDIR (
  echo [ERROR] Visual Studio 2022 not found.
  echo         Install "Visual Studio 2022 Build Tools" with the C++ workload.
  pause
  exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [ERROR] vcvars64.bat failed.
  pause
  exit /b 1
)

nmake /nologo /f makefile
if errorlevel 1 (
  echo.
  echo [FAILED] build error.
  pause
  exit /b 1
)

rem makefile 把 H.dll 生成在本目录；安装用的是上一级目录（D:\H）那份，这里同步过去
copy /y "%~dp0H.dll" "%~dp0..\H.dll" >nul
if errorlevel 1 (
  echo [WARN] built, but could not copy H.dll to the parent folder.
)

echo.
echo [OK] H.dll built.
pause
