cmake_minimum_required(VERSION 3.16)

project(CalendarServer
    VERSION 1.3.0
    DESCRIPTION "Sync server of the calendar planner"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Only the console half of Qt: the server has no GUI and runs on a headless
# Linux box as well as on Windows. Qt 6.2 is what current LTS distributions ship.
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Network Sql)

set(SERVER_CORE_SOURCES
    src/httpserver.cpp
    src/httpserver.h
    src/storage.cpp
    src/storage.h
    src/api.cpp
    src/api.h
    src/shares.cpp
    src/shares.h
    src/config.cpp
    src/config.h
    src/runtimefile.cpp
    src/runtimefile.h
    src/service.cpp
    src/service.h
    src/workerpool.cpp
    src/workerpool.h
    src/metrics.cpp
    src/metrics.h
    src/server.cpp
    src/server.h)

# Compiled once for the server, its tests and the admin panel, which reads the
# same base with the same code.
add_library(server_core STATIC ${SERVER_CORE_SOURCES})
target_include_directories(server_core PUBLIC src)
target_link_libraries(server_core PUBLIC Qt6::Core Qt6::Network Qt6::Sql)
target_compile_definitions(server_core PUBLIC
    QT_DISABLE_DEPRECATED_BEFORE=0x060000
    SERVER_VERSION="${PROJECT_VERSION}")
if(WIN32)
    # The service control manager; the memory of the process for the metrics.
    target_link_libraries(server_core PUBLIC advapi32 psapi)
endif()

add_executable(CalendarServer src/main.cpp)
target_link_libraries(CalendarServer PRIVATE server_core)

include(GNUInstallDirs)
install(TARGETS CalendarServer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The benchmark: it measures the machine and picks the load settings for it,
# running the real server on a base of made-up accounts. Installed with the
# server; the admin panel starts it from its page "Многопоточность".
set(BENCH_SOURCES
    bench/basemaker.cpp
    bench/basemaker.h
    bench/loadrun.cpp
    bench/loadrun.h
    bench/probe.cpp
    bench/probe.h
    bench/reporter.cpp
    bench/reporter.h
    bench/tuner.cpp
    bench/tuner.h)

add_library(bench_core STATIC ${BENCH_SOURCES})
target_include_directories(bench_core PUBLIC bench)
target_link_libraries(bench_core PUBLIC server_core)

add_executable(CalendarServerBench bench/main.cpp)
target_link_libraries(CalendarServerBench PRIVATE bench_core)
install(TARGETS CalendarServerBench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# What the admin panel knows without a window: its queries over the base, the
# backups, the service and the records of the planner. The tests use it too.
set(ADMIN_DATA_SOURCES
    admin/admindb.cpp
    admin/admindb.h
    admin/adminsession.cpp
    admin/adminsession.h
    admin/backups.cpp
    admin/backups.h
    admin/benchmarkrunner.cpp
    admin/benchmarkrunner.h
    admin/inifile.cpp
    admin/inifile.h
    admin/loadhistory.cpp
    admin/loadhistory.h
    admin/records.cpp
    admin/records.h
    admin/servicecontrol.cpp
    admin/servicecontrol.h)

add_library(admin_data STATIC ${ADMIN_DATA_SOURCES})
target_include_directories(admin_data PUBLIC admin)
target_link_libraries(admin_data PUBLIC server_core bench_core)
if(WIN32)
    target_link_libraries(admin_data PUBLIC shell32)
endif()

# The admin panel itself needs Qt Widgets, which a headless box usually does
# not have; without them only the server and the tests are built.
option(SERVER_BUILD_ADMIN "Build the admin panel (needs Qt Widgets)" ON)
if(SERVER_BUILD_ADMIN)
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(NOT Qt6Widgets_FOUND)
        message(STATUS "Qt Widgets not found: the admin panel is not built")
    endif()
endif()

if(SERVER_BUILD_ADMIN AND Qt6Widgets_FOUND)
    add_executable(CalendarServerAdmin WIN32
        admin/main.cpp
        admin/backuppage.cpp
        admin/backuppage.h
        admin/chart.cpp
        admin/chart.h
        admin/loadpage.cpp
        admin/loadpage.h
        admin/logpage.cpp
        admin/logpage.h
        admin/look.cpp
        admin/look.h
        admin/mainwindow.cpp
        admin/mainwindow.h
        admin/page.cpp
        admin/page.h
        admin/serverpage.cpp
        admin/serverpage.h
        admin/statspage.cpp
        admin/statspage.h
        admin/threadspage.cpp
        admin/threadspage.h
        admin/userspage.cpp
        admin/userspage.h)
    target_link_libraries(CalendarServerAdmin PRIVATE admin_data Qt6::Widgets)
    if(WIN32)
        # The icon and the name Explorer shows. The build follows the .rc,
        # not the icon it pulls in, hence the dependency by hand.
        target_sources(CalendarServerAdmin PRIVATE admin/admin.rc)
        set_source_files_properties(admin/admin.rc PROPERTIES
            OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/CalendarServer.ico)
    endif()
    install(TARGETS CalendarServerAdmin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

option(SERVER_BUILD_TESTS "Build the unit tests" ON)

if(SERVER_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)

    add_executable(tst_server tests/tst_server.cpp)
    target_link_libraries(tst_server PRIVATE server_core Qt6::Test)
    add_test(NAME server COMMAND tst_server)

    add_executable(tst_admin tests/tst_admin.cpp)
    target_link_libraries(tst_admin PRIVATE admin_data Qt6::Test)
    add_test(NAME admin COMMAND tst_admin)

    add_executable(tst_bench tests/tst_bench.cpp)
    target_link_libraries(tst_bench PRIVATE bench_core Qt6::Test)
    add_test(NAME bench COMMAND tst_bench)
endif()
