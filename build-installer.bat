@echo off
rem ---------------------------------------------------------------------------
rem  Installers of the sync server for 64-bit Windows, built with WiX 3:
rem
rem      CalendarServer-<version>-x64.msi     the Windows Installer package
rem      CalendarServer-<version>-setup.exe   the same, wrapped into setup.exe
rem
rem      build-installer.bat                  into build-release\installer
rem      build-installer.bat -out <folder>    into a folder of your choice
rem      build-installer.bat -noui            do not wait for a key at the end
rem      build-installer.bat -skipbuild       pack the server already built in
rem                                           SERVER_BUILD (build-packages.ps1
rem                                           builds it as a step of its own)
rem
rem  It builds in build-release\ rather than build\: the server that is in use
rem  usually runs from build\, and Windows does not let a running exe be
rem  linked anew. SERVER_BUILD overrides the folder. The server and the admin
rem  panel are then laid out with windeployqt, harvested with heat and packed:
rem  the server as the Windows service, the panel with its shortcuts. The
rem  version comes from project() in CMakeLists.txt, the sources of the
rem  packages are in packaging\windows, WiX is found through SERVER_WIX
rem  (build-env.bat).
rem ---------------------------------------------------------------------------

chcp 65001 >nul
setlocal
cd /d "%~dp0"
rem Kept before the loop below: shift moves %0 along with the arguments.
set "HERE=%~dp0"

set "NOUI="
set "OUT="
set "SKIPBUILD="
:args
if "%~1"=="" goto :argsdone
if /i "%~1"=="-noui" set "NOUI=1"
if /i "%~1"=="-skipbuild" set "SKIPBUILD=1"
if /i "%~1"=="-out" (
    set "OUT=%~f2"
    shift
)
shift
goto :args
:argsdone

if not defined SERVER_BUILD set "SERVER_BUILD=build-release"
call "%HERE%build-env.bat" || goto :fail
if not defined OUT set "OUT=%HERE%%SERVER_BUILD%\installer"

if not exist "%SERVER_WIX%\candle.exe" (
    echo [ОШИБКА] Не найден WiX 3: "%SERVER_WIX%"
    echo           Установите WiX Toolset 3 или задайте SERVER_WIX - папку с candle.exe.
    goto :fail
)
if not exist "%SERVER_QT%\bin\windeployqt.exe" (
    echo [ОШИБКА] Не найден windeployqt в наборе Qt: "%SERVER_QT%"
    goto :fail
)

set "VERSION="
for /f "tokens=2" %%V in ('findstr /r /c:"^ *VERSION [0-9]" "%HERE%CMakeLists.txt"') do (
    if not defined VERSION set "VERSION=%%V"
)
if not defined VERSION (
    echo [ОШИБКА] Не нашлась версия в project^(^) файла CMakeLists.txt.
    goto :fail
)

set "WORK=%HERE%build-installer"
set "MSI=CalendarServer-%VERSION%-x64.msi"
set "SETUP=CalendarServer-%VERSION%-setup.exe"
set "PKG=%HERE%packaging\windows"

echo.
echo ============================================================
echo   Установщики сервера %VERSION%   %DATE% %TIME:~0,8%
echo   Сборка: %SERVER_BUILD%
echo   WiX:    %SERVER_WIX%
echo   В:      %OUT%
echo ============================================================

rem ---- 1. build and tests -----------------------------------------------
title Сервер: сборка
echo.
if defined SKIPBUILD (
    echo [1/4] Сборка пропущена: беру готовый %SERVER_EXE%.
) else (
    echo [1/4] Сборка и тесты ^(build-server.bat^)...
    call "%HERE%build-server.bat" -noui || goto :fail
)
if not exist "%HERE%%SERVER_EXE%" (
    echo [ОШИБКА] Сервер не собран: "%HERE%%SERVER_EXE%"
    goto :fail
)
set "ADMIN_EXE=%SERVER_BUILD%\CalendarServerAdmin.exe"
set "BENCH_EXE=%SERVER_BUILD%\CalendarServerBench.exe"
if not exist "%HERE%%BENCH_EXE%" (
    echo [ОШИБКА] Бенчмарк не собран: "%HERE%%BENCH_EXE%"
    goto :fail
)
if not exist "%HERE%%ADMIN_EXE%" (
    echo [ОШИБКА] Админ-панель не собрана: "%HERE%%ADMIN_EXE%"
    echo           Ей нужен Qt Widgets в наборе "%SERVER_QT%".
    goto :fail
)

rem ---- 2. what goes into the package ------------------------------------
rem --compiler-runtime brings the MinGW libraries along, without --release
rem for the same reason as in the planner: with a MinGW build it drops every
rem plugin, the SQLite driver among them, and the server cannot open its base.
rem The panel is laid out into the same folder: it adds Widgets and the
rem platform plugin to what the server needs.
rem
rem CalendarServer.exe then moves out of the folder heat harvests: it is the
rem file of the service component in CalendarServer.wxs, which a harvested
rem component could not carry.
title Сервер: файлы пакета
echo.
echo [2/4] Раскладка Qt и файлы пакета...
if exist "%WORK%" rmdir /s /q "%WORK%"
mkdir "%WORK%\files" || goto :fail
mkdir "%WORK%\service" || goto :fail
copy /y "%HERE%%SERVER_EXE%" "%WORK%\files\" >nul || goto :fail
copy /y "%HERE%%ADMIN_EXE%" "%WORK%\files\" >nul || goto :fail
rem The benchmark needs what the server needs (Core, Network, Sql): the
rem libraries windeployqt lays out for the server are there for it as well.
copy /y "%HERE%%BENCH_EXE%" "%WORK%\files\" >nul || goto :fail
copy /y "%HERE%deploy\server.ini.example" "%WORK%\files\" >nul || goto :fail
"%SERVER_QT%\bin\windeployqt.exe" --compiler-runtime --no-translations ^
    "%WORK%\files\CalendarServer.exe" >nul || goto :fail
"%SERVER_QT%\bin\windeployqt.exe" --compiler-runtime --no-translations --no-opengl-sw ^
    --no-system-d3d-compiler "%WORK%\files\CalendarServerAdmin.exe" >nul || goto :fail
if not exist "%WORK%\files\sqldrivers\qsqlite.dll" (
    echo [ОШИБКА] windeployqt не положил драйвер SQLite - сервер не откроет базу.
    goto :fail
)
if not exist "%WORK%\files\platforms\qwindows.dll" (
    echo [ОШИБКА] windeployqt не положил qwindows.dll - админ-панель не откроет окно.
    goto :fail
)
move /y "%WORK%\files\CalendarServer.exe" "%WORK%\service\" >nul || goto :fail
if not exist "%OUT%" mkdir "%OUT%" || goto :fail
del /q "%OUT%\%MSI%" "%OUT%\%SETUP%" 2>nul

rem ---- 3. the MSI -------------------------------------------------------
title Сервер: MSI
echo.
echo [3/4] %MSI%...
"%SERVER_WIX%\heat.exe" dir "%WORK%\files" -nologo -cg ServerFiles -dr INSTALLFOLDER ^
    -srd -sreg -scom -ag -var var.SourceDir -out "%WORK%\files.wxs" || goto :fail
"%SERVER_WIX%\candle.exe" -nologo -arch x64 -dVersion=%VERSION% -dSourceDir="%WORK%\files" ^
    -dServiceExe="%WORK%\service\CalendarServer.exe" -dIconFile="%PKG%\CalendarServer.ico" ^
    -dPackagingDir="%PKG%" -ext WixUIExtension -ext WixUtilExtension -ext WixFirewallExtension ^
    -out "%WORK%\\" "%PKG%\CalendarServer.wxs" "%WORK%\files.wxs" || goto :fail
rem The firewall extension of WiX 3.7 has no Russian strings: firewall-ru.wxl.
"%SERVER_WIX%\light.exe" -nologo -cultures:ru-ru -ext WixUIExtension -ext WixUtilExtension ^
    -ext WixFirewallExtension -loc "%PKG%\firewall-ru.wxl" -spdb -out "%OUT%\%MSI%" ^
    "%WORK%\CalendarServer.wixobj" "%WORK%\files.wixobj" || goto :fail

rem ---- 4. setup.exe -----------------------------------------------------
title Сервер: setup.exe
echo.
echo [4/4] %SETUP%...
"%SERVER_WIX%\candle.exe" -nologo -dVersion=%VERSION% -dMsiFile="%OUT%\%MSI%" ^
    -dPackagingDir="%PKG%" -ext WixBalExtension -ext WixUtilExtension ^
    -out "%WORK%\CalendarServerSetup.wixobj" "%PKG%\CalendarServerSetup.wxs" || goto :fail
"%SERVER_WIX%\light.exe" -nologo -ext WixBalExtension -ext WixUtilExtension ^
    -spdb -out "%OUT%\%SETUP%" "%WORK%\CalendarServerSetup.wixobj" || goto :fail

echo.
echo ============================================================
echo   ГОТОВО:
echo     %OUT%\%MSI%
echo     %OUT%\%SETUP%
echo ============================================================
echo   Пакеты не подписаны. Сервер ставится службой CosmosCalendarServer;
echo   его база при удалении остаётся.
echo.
title Сервер: установщики готовы
if not defined NOUI pause
exit /b 0

:fail
echo.
echo ============================================================
echo   УСТАНОВЩИКИ НЕ СОБРАНЫ
echo ============================================================
title Сервер: установщики не собраны
if not defined NOUI pause
exit /b 1
