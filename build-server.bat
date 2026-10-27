@echo off
rem ---------------------------------------------------------------------------
rem  Release build of the sync server: build\CalendarServer.exe, the admin
rem  panel build\CalendarServerAdmin.exe, the tests of both and their run.
rem
rem      build-server.bat            build, then wait for a key press
rem      build-server.bat -noui      build and return at once (for scripts)
rem
rem  The server has no deployment step of its own: it is started from the build
rem  folder by run-server.bat, which puts Qt on PATH for it.
rem ---------------------------------------------------------------------------

chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "NOUI="
for %%A in (%*) do if /i "%%~A"=="-noui" set "NOUI=1"

call "%~dp0build-env.bat" || goto :fail

if not exist "%SERVER_CMAKE%" (
    echo [ОШИБКА] Не найден cmake: "%SERVER_CMAKE%"
    echo           Задайте SERVER_QT_TOOLS или SERVER_CMAKE.
    goto :fail
)
if not exist "%SERVER_QT%\bin\Qt6Core.dll" (
    echo [ОШИБКА] Не найден набор Qt: "%SERVER_QT%"
    echo           Задайте SERVER_QT.
    goto :fail
)

echo.
echo ============================================================
echo   Сборка сервера   %DATE% %TIME:~0,8%
echo   Qt: %SERVER_QT%
echo ============================================================

rem ---- 1. configure -----------------------------------------------------
title Сервер: настройка CMake
echo.
echo [1/3] Настройка CMake...
if exist "%SERVER_BUILD%\CMakeCache.txt" call :checkcache "%SERVER_BUILD%" || goto :fail
if exist "%SERVER_BUILD%\CMakeCache.txt" (
    echo       %SERVER_BUILD% уже настроен, пропускаю.
) else (
    "%SERVER_CMAKE%" -S . -B "%SERVER_BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_PREFIX_PATH="%SERVER_QT%" -DSERVER_BUILD_TESTS=ON || goto :fail
)

rem ---- 2. compile -------------------------------------------------------
title Сервер: сборка
echo.
echo [2/3] Сборка...
"%SERVER_CMAKE%" --build "%SERVER_BUILD%" || goto :fail

rem ---- 3. tests ---------------------------------------------------------
rem QtTest prints nothing into this console on Windows (the process just ends
rem with code 0), so the result is read back from a file. The tests raise a
rem server of their own on a spare port; nothing else needs to be running.
title Сервер: тесты
echo.
echo [3/3] Тесты...
call :runtests tst_server || goto :fail
call :runtests tst_admin || goto :fail
call :runtests tst_bench || goto :fail

echo.
echo ============================================================
echo   ГОТОВО: %SERVER_BUILD%\CalendarServer.exe
echo           %SERVER_BUILD%\CalendarServerAdmin.exe
echo           %SERVER_BUILD%\CalendarServerBench.exe
echo ============================================================
echo   Запуск: run-server.bat; админ-панель: CalendarServerAdmin.exe -d ^<папка^>
echo.
title Сервер: сборка готова
if not defined NOUI pause
exit /b 0

:fail
echo.
echo ============================================================
echo   СБОРКА СЕРВЕРА НЕ УДАЛАСЬ
echo ============================================================
title Сервер: сборка не удалась
if not defined NOUI pause
exit /b 1

:runtests
rem %1 - the test program in the build folder.
set "TESTLOG=%TEMP%\calendarserver-%~1.txt"
del /q "%TESTLOG%" 2>nul
"%SERVER_BUILD%\%~1.exe" -o "%TESTLOG%,txt"
if not exist "%TESTLOG%" (
    echo [ОШИБКА] %~1 не запустился - отчёта нет.
    exit /b 1
)
findstr /c:"Totals:" "%TESTLOG%"
findstr /c:"FAIL!" "%TESTLOG%" >nul && (
    echo.
    echo [ОШИБКА] Есть упавшие тесты в %~1:
    findstr /c:"FAIL!" "%TESTLOG%"
    exit /b 1
)
exit /b 0

:checkcache
rem %1 - каталог сборки. Кэш CMake помнит абсолютный путь, на котором его
rem создали, поэтому каталог, переехавший вместе с исходниками, непригоден:
rem cmake останавливается на "is different than the directory ... where
rem CMakeCache.txt was created", и сборка падает на кэше, а не на чём-то
rem настоящем. Починить такой кэш нельзя, только выбросить.
rem
rem Выбрасывается именно кэш, а не весь каталог: рядом с ним лежит поставка,
rem разложенная windeployqt, и пересобрать её отсюда нечем.
set "CACHED="
for /f "usebackq tokens=2 delims==" %%H in (`findstr /b /c:"CMAKE_HOME_DIRECTORY:INTERNAL=" "%~1\CMakeCache.txt"`) do set "CACHED=%%H"
if not defined CACHED exit /b 0
set "CACHED=%CACHED:/=\%"
if /i "%CACHED%"=="%CD%" exit /b 0
echo       Каталог "%~1" остался от другого размещения исходников.
echo         в кэше: %CACHED%
echo         сейчас: %CD%
echo       Убираю кэш CMake и настраиваю заново.
del /q "%~1\CMakeCache.txt" "%~1\build.ninja" "%~1\cmake_install.cmake" 2>nul
del /q "%~1\CTestTestfile.cmake" "%~1\.ninja_deps" "%~1\.ninja_log" 2>nul
rd /s /q "%~1\CMakeFiles" 2>nul
for /d %%D in ("%~1\*_autogen") do rd /s /q "%%D" 2>nul
if exist "%~1\CMakeCache.txt" (
    echo [ОШИБКА] Не удалось убрать кэш в "%~1".
    echo           Обычно файлы держит запущенная программа из этого каталога:
    echo           закройте её и повторите.
    exit /b 1
)
exit /b 0
