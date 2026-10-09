@echo off
rem ====================================================================
rem  H plugin - rebuild H.dat so it points to THIS folder.
rem  Run this after you copy/extract the H folder to a new location.
rem  Only ASCII characters appear in this file, so it is safe anywhere.
rem ====================================================================
setlocal
cd /d "%~dp0"

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"

echo NAME			H> H.dat
echo startup			dll>> H.dat
echo toolkit			object>> H.dat
echo exec_file %HERE%\H.dll>> H.dat
echo text_dir %HERE%\text>> H.dat
echo description H - drawing balloon numbering tool>> H.dat
echo allow_stop		true>> H.dat
echo delay_start		false>> H.dat
echo END>> H.dat

echo.
echo ============================================================
echo    完成！插件位置已记录为下面这个文件夹：
echo       %HERE%
echo ============================================================
echo.
echo    最后只剩一步，让 Creo 认识它：
echo.
echo       1) 打开 Creo
echo       2) 顶部【工具】选项卡 → 【辅助应用程序】
echo       3) 点【注册】，选中本文件夹里的 H.dat，打开
echo       4) 列表里选中 H，点【启动】，再勾上【自动启动】
echo.
echo    更细的图文步骤，请看同目录的：安装说明_超简单版.txt
echo.
pause
