@echo off
rem ---------------------------------------------------------------------------
rem  Runs the sync server in this console and keeps it in the foreground: the
rem  window is the log, Ctrl+C stops it.
rem
rem      run-server.bat                      порт 8765, база в %APPDATA%
rem      run-server.bat -p 8766              другой порт
rem      run-server.bat -d D:\srv-data       своя папка данных (для проверок)
rem      run-server.bat --list-users         показать аккаунты и выйти
rem      run-server.bat --no-registration    запретить создание аккаунтов
rem      run-server.bat -noui                не ждать клавишу после выхода
rem
rem  Everything except -noui is handed to the server as it is; --help lists the
rem  rest. Test runs deserve a --data-dir of their own, so that test accounts
rem  stay out of the real database.
rem ---------------------------------------------------------------------------

chcp 65001 >nul
setlocal
cd /d "%~dp0"
rem Kept before the loop below: shift moves %0 along with the arguments,
rem so %~dp0 stops naming this script once parsing has started.
set "HERE=%~dp0"

rem -noui is ours, the rest belongs to the server. Rebuilt with quotes around
rem each argument, which keeps paths with spaces in one piece.
set "NOUI="
set "ARGS="
set "OWNSPOT="
set "NOLISTEN="
:parse
if "%~1"=="" goto :ready
if /i "%~1"=="-noui" (set "NOUI=1") else (set ARGS=%ARGS% "%~1")
rem A port or a data folder of its own means a second server on purpose -
rem a test run beside the working one - so the guard below only warns.
if /i "%~1"=="-p" set "OWNSPOT=1"
if /i "%~1"=="--port" set "OWNSPOT=1"
if /i "%~1"=="-d" set "OWNSPOT=1"
if /i "%~1"=="--data-dir" set "OWNSPOT=1"
rem These answer and quit without listening at all, so nothing can clash.
if /i "%~1"=="--list-users" set "NOLISTEN=1"
if /i "%~1"=="--delete-user" set "NOLISTEN=1"
if /i "%~1"=="--set-password" set "NOLISTEN=1"
if /i "%~1"=="--help" set "NOLISTEN=1"
if /i "%~1"=="-h" set "NOLISTEN=1"
if /i "%~1"=="--version" set "NOLISTEN=1"
if /i "%~1"=="-v" set "NOLISTEN=1"
shift
goto :parse
:ready

call "%HERE%build-env.bat" || goto :fail

if not exist "%SERVER_EXE%" (
    echo [ОШИБКА] Сервер не собран: "%SERVER_EXE%"
    echo           Соберите его: build-server.bat
    goto :fail
)

rem Two servers on one port and one database do not end well: the second fails
rem to listen and the mistake is easy to miss in the log.
if defined NOLISTEN goto :launch
tasklist /fi "imagename eq CalendarServer.exe" 2>nul | findstr /i "CalendarServer.exe" >nul
if not errorlevel 1 (
    if defined OWNSPOT (
        echo [ВНИМАНИЕ] Один сервер уже работает. Запускаю второй, раз указаны
        echo            свой порт или своя папка данных.
    ) else (
        echo [ОШИБКА] Сервер уже запущен. Остановите прежний экземпляр
        echo           или запустите этот на другом порту: run-server.bat -p 8766
        goto :fail
    )
)

:launch
echo.
echo ============================================================
echo   Сервер синхронизации   %DATE% %TIME:~0,8%
echo   Ctrl+C - остановить
echo ============================================================
echo.
title Сервер: работает

"%SERVER_EXE%"%ARGS%
set "CODE=%ERRORLEVEL%"

echo.
echo ============================================================
echo   Сервер остановлен, код %CODE%
echo ============================================================
title Сервер: остановлен
if not defined NOUI pause
exit /b %CODE%

:fail
echo.
title Сервер: не запущен
if not defined NOUI pause
exit /b 1
