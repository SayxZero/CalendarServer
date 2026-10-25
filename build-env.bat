@echo off
rem ---------------------------------------------------------------------------
rem  Where the build tools live on this machine.
rem
rem  Every path can be overridden from outside without touching this file:
rem      set "SERVER_QT=C:\Qt\6.5.3\mingw_64"
rem      build-server.bat
rem
rem  The names start with SERVER_ on purpose: QT_DIR and the like are common
rem  CMake hints and are often already set to another version of Qt entirely.
rem
rem  The server needs only the console half of Qt - Core, Network, Sql and Test.
rem  No GUI kit, no Android kit, no JDK.
rem
rem  This script only sets variables and extends PATH; it builds nothing.
rem ---------------------------------------------------------------------------

if not defined SERVER_QT        set "SERVER_QT=D:\Qt\6.5.3\mingw_64"
if not defined SERVER_QT_TOOLS  set "SERVER_QT_TOOLS=D:\Qt\Tools"
if not defined SERVER_MINGW     set "SERVER_MINGW=%SERVER_QT_TOOLS%\mingw1120_64"
if not defined SERVER_NINJA     set "SERVER_NINJA=%SERVER_QT_TOOLS%\Ninja"
if not defined SERVER_CMAKE     set "SERVER_CMAKE=%SERVER_QT_TOOLS%\CMake_64\bin\cmake.exe"

rem Where the built server ends up, and where it keeps server.db when no
rem --data-dir is given: %APPDATA%\CalendarPlanner\CalendarServer.
if not defined SERVER_BUILD     set "SERVER_BUILD=build"
if not defined SERVER_EXE       set "SERVER_EXE=%SERVER_BUILD%\CalendarServer.exe"

set "PATH=%SERVER_QT%\bin;%SERVER_MINGW%\bin;%SERVER_NINJA%;%PATH%"
exit /b 0
