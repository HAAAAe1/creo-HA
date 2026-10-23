@echo off
chcp 936 >nul
setlocal enabledelayedexpansion
set "HDLL=%~dp0..\H.dll"
if not exist "%HDLL%" set "HDLL=%~dp0H.dll"
cd /d "%~dp0"

echo ==============================================================
echo    H 插件  ·  解锁工具   （打包发给别人之前必须做）
echo ==============================================================
echo.
echo  用 TOOLKIT 编译出来的 H.dll 默认是"锁定"的，只有装了
echo  TOOLKIT 开发许可的电脑能加载。别人打开 Creo 会弹：
echo      Creo Toolkit 应用程序 "H" 在分配到您的位置之前未被解锁
echo.
echo  本工具给 H.dll 盖章解锁，之后任何人的 Creo 都能直接加载。
echo  重要：每次重新编译 H.dll 之后，都要再跑一次。
echo.
echo  用法（一般直接双击就行）：
echo      unlock.bat
echo      unlock.bat 解锁器路径 许可证文件      两个都可以不给
echo ==============================================================
echo.

if not exist "%HDLL%" (
    echo [错误] 没找到 H.dll，请先编译。
    pause
    exit /B 1
)

"%SystemRoot%\System32	asklist.exe" /FI "IMAGENAME eq xtop.exe" 2>nul | "%SystemRoot%\System32ind.exe" /i "xtop.exe" >nul
if not errorlevel 1 (
    echo [错误] Creo 正在运行，H.dll 被它占用，解不了锁。
    echo        请先【完全退出 Creo】，再双击本文件。
    pause
    exit /B 1
)

if not "%~2"=="" (
    set "PTC_D_LICENSE_FILE=%~2"
    echo  许可证： %~2
)

rem ---- 收集候选解锁器（Creo 自带 protk_unlock.bat）----
set "LIST=%TEMP%\h_unlock_list.txt"
if exist "%LIST%" del "%LIST%" >nul 2>&1
if not "%~1"=="" echo %~1>>"%LIST%"
if not "%~1"=="" goto have_list

for %%R in (
    "D:\creo1"
    "E:\creo1"
    "C:\Program Files\PTC"
    "C:\Program Files (x86)\PTC"
    "D:\Program Files\PTC"
    "E:\Program Files\PTC"
    "D:\PTC"
    "C:\PTC"
) do (
    if exist "%%~R" (
        for /d %%V in ("%%~R\Creo*") do (
            if exist "%%~V\Parametric\bin\protk_unlock.bat" echo %%~V\Parametric\bin\protk_unlock.bat>>"%LIST%"
            for /d %%W in ("%%~V\Creo*") do (
                if exist "%%~W\Parametric\bin\protk_unlock.bat" echo %%~W\Parametric\bin\protk_unlock.bat>>"%LIST%"
            )
        )
    )
)
:have_list

if not exist "%LIST%" (
    echo [错误] 没有找到 Creo 自带的解锁工具 protk_unlock.bat
    echo.
    echo  办法：到 Creo 安装目录里找到
    echo        ...\Parametric\bin\protk_unlock.bat
    echo        把它【拖到本文件上】松手，就会用它来解锁。
    echo.
    pause
    exit /B 1
)

rem ---- 关键：先复制到纯英文临时目录 ----
rem protk_unlock 处理不了中文路径（实测会截断成 D:\H\ 并报 ERROR opening）
set "WORK=%TEMP%\h_unlock_work"
if not exist "%WORK%" mkdir "%WORK%" >nul 2>&1
copy /y "%HDLL%" "%WORK%\H.dll" >nul
if errorlevel 1 (
    echo [错误] 复制到临时目录失败： %WORK%
    pause
    exit /B 1
)

set "RC=1"
for /f "usebackq delims=" %%L in ("%LIST%") do (
    if "!RC!"=="1" (
        echo.
        echo  试解锁器： %%L
        call "%%L" "%WORK%\H.dll"
        set "RC=!ERRORLEVEL!"
        if "!RC!"=="0" (
            echo  [成功] %%L
        ) else (
            echo  [不行] 返回码 !RC!
        )
    )
)

echo.
if "%RC%"=="0" (
    copy /y "%WORK%\H.dll" "%HDLL%" >nul
    echo ==============================================================
    echo    解锁成功！H.dll 现在可以发给任何人了。
    for %%A in ("%HDLL%") do echo    文件： %%~fA
    echo ==============================================================
) else (
    echo ==============================================================
    echo    解锁失败（最后一个返回码 %RC%）
    echo.
    echo    依次检查：
    echo      1. Creo 是否真的完全退出了（任务管理器里没有 xtop.exe）
    echo      2. 这台电脑是否有 TOOLKIT 开发许可；
    echo         报 "requires option TOOLKIT-for-3D_Drawings" 就是缺 222
    echo         报 "prolicense error ... ERROR opening" 多是路径含中文
    echo      3. 许可版本要跟解锁器版本匹配（见文档）
    echo ==============================================================
)
echo.
pause
endlocal & exit /B %RC%
