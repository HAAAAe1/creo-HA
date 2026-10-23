@echo off
chcp 936 >nul
setlocal
set "HDLL=%~dp0..\H.dll"
if not exist "%HDLL%" set "HDLL=%~dp0H.dll"
cd /d "%~dp0"

echo ==============================================================
echo    H 插件  ·  解锁工具   （打包发给别人之前必须做）
echo ==============================================================
echo.
echo  用 TOOLKIT 编译出来的 H.dll 默认是"锁定"的，只有装了
echo  TOOLKIT 开发许可的电脑（也就是你这台开发机）能加载。
echo  别人打开 Creo 会弹：
echo      Creo Toolkit 应用程序 "H" 在分配到您的位置之前未被解锁
echo.
echo  本工具给 H.dll 盖章解锁，之后任何人的 Creo 都能直接加载。
echo.
echo  重要：每次重新编译 H.dll 之后，都要再跑一次本工具。
echo ==============================================================
echo.

if not exist "%HDLL%" (
    echo [错误] 本文件夹里没有 H.dll，请先编译。
    echo.
    pause
    exit /B 1
)

tasklist /FI "IMAGENAME eq xtop.exe" 2>nul | find /i "xtop.exe" >nul
if not errorlevel 1 (
    echo [错误] Creo 正在运行，H.dll 被它占用，解不了锁。
    echo        请先【完全退出 Creo】，再双击本文件。
    echo.
    pause
    exit /B 1
)

set "UNLOCK="
if not "%~1"=="" set "UNLOCK=%~1"

if not defined UNLOCK (
    for %%R in (
        "C:\Program Files\PTC"
        "C:\Program Files (x86)\PTC"
        "D:\Program Files\PTC"
        "D:\PTC"
        "D:\creo1"
        "E:\Program Files\PTC"
    ) do (
        if not defined UNLOCK (
            if exist "%%~R" (
                for /d %%V in ("%%~R\Creo*") do (
                    if not defined UNLOCK (
                        if exist "%%~V\Parametric\bin\protk_unlock.bat" set "UNLOCK=%%~V\Parametric\bin\protk_unlock.bat"
                    )
                    if not defined UNLOCK (
                        for /d %%W in ("%%~V\Creo*") do (
                            if not defined UNLOCK (
                                if exist "%%~W\Parametric\bin\protk_unlock.bat" set "UNLOCK=%%~W\Parametric\bin\protk_unlock.bat"
                            )
                        )
                    )
                )
            )
        )
    )
)

if not defined UNLOCK (
    echo [错误] 没有自动找到 Creo 自带的解锁工具 protk_unlock.bat
    echo.
    echo  解决办法：
    echo    到 Creo 安装目录里找到
    echo        ...\Parametric\bin\protk_unlock.bat
    echo    把它【拖到本文件上】松手，就会用它来解锁。
    echo.
    pause
    exit /B 1
)

echo  解锁工具： %UNLOCK%
echo  目标文件： %~dp0H.dll
echo.
echo  正在解锁（会占用一个 TOOLKIT 许可，约几秒）...
echo.

call "%UNLOCK%" "%HDLL%"
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
    echo ==============================================================
    echo    解锁成功！H.dll 现在可以发给任何人了。
    echo ==============================================================
) else (
    echo ==============================================================
    echo    解锁失败，返回码 %RC%
    echo.
    echo    依次检查：
    echo      1. Creo 是否真的完全退出了（任务管理器里没有 xtop.exe）
    echo      2. 这台电脑是否有 Creo Parametric TOOLKIT 开发许可
    echo      3. 许可服务器是否在运行
    echo ==============================================================
)
echo.
pause
endlocal & exit /B %RC%
