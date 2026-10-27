# qmake build of the benchmark; CMakeLists.txt describes the same program.
# It compiles the server's own sources: it makes the base the server reads.
QT = core network sql
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = CalendarServerBench
VERSION = 1.3.0
DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000 SERVER_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += ../src .

SOURCES += \
    main.cpp \
    basemaker.cpp \
    loadrun.cpp \
    probe.cpp \
    reporter.cpp \
    tuner.cpp \
    ../src/httpserver.cpp \
    ../src/storage.cpp \
    ../src/api.cpp \
    ../src/shares.cpp \
    ../src/config.cpp \
    ../src/runtimefile.cpp \
    ../src/service.cpp \
    ../src/workerpool.cpp \
    ../src/metrics.cpp \
    ../src/server.cpp

HEADERS += \
    basemaker.h \
    loadrun.h \
    probe.h \
    reporter.h \
    tuner.h \
    ../src/httpserver.h \
    ../src/storage.h \
    ../src/api.h \
    ../src/shares.h \
    ../src/config.h \
    ../src/runtimefile.h \
    ../src/service.h \
    ../src/workerpool.h \
    ../src/metrics.h \
    ../src/server.h

win32: LIBS += -ladvapi32 -lpsapi

unix {
    target.path = /usr/local/bin
    INSTALLS += target
}
