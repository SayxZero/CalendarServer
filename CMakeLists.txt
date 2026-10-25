cmake_minimum_required(VERSION 3.16)

project(CalendarServer
    VERSION 1.0.0
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
    src/config.h)

add_executable(CalendarServer src/main.cpp ${SERVER_CORE_SOURCES})
target_include_directories(CalendarServer PRIVATE src)
target_link_libraries(CalendarServer PRIVATE Qt6::Core Qt6::Network Qt6::Sql)
target_compile_definitions(CalendarServer PRIVATE
    QT_DISABLE_DEPRECATED_BEFORE=0x060000
    SERVER_VERSION="${PROJECT_VERSION}")

include(GNUInstallDirs)
install(TARGETS CalendarServer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

option(SERVER_BUILD_TESTS "Build the unit tests" ON)

if(SERVER_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)

    add_executable(tst_server tests/tst_server.cpp ${SERVER_CORE_SOURCES})
    target_include_directories(tst_server PRIVATE src)
    target_link_libraries(tst_server PRIVATE Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
    target_compile_definitions(tst_server PRIVATE
        QT_DISABLE_DEPRECATED_BEFORE=0x060000
        SERVER_VERSION="${PROJECT_VERSION}")

    add_test(NAME server COMMAND tst_server)
endif()
