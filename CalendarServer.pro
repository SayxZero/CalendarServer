# qmake build of the server; CMakeLists.txt describes the same program.
QT = core network sql
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = CalendarServer
VERSION = 1.0.0
DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000 SERVER_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += src

SOURCES += \
    src/main.cpp \
    src/httpserver.cpp \
    src/storage.cpp \
    src/api.cpp \
    src/shares.cpp \
    src/config.cpp

HEADERS += \
    src/httpserver.h \
    src/storage.h \
    src/api.h \
    src/shares.h \
    src/config.h

unix {
    target.path = /usr/local/bin
    INSTALLS += target
}
