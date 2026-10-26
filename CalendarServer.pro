# qmake build of the server; CMakeLists.txt describes the same program. The
# admin panel has a file of its own: admin/CalendarServerAdmin.pro.
QT = core network sql
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = CalendarServer
VERSION = 1.2.0
DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000 SERVER_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += src

SOURCES += \
    src/main.cpp \
    src/httpserver.cpp \
    src/storage.cpp \
    src/api.cpp \
    src/shares.cpp \
    src/config.cpp \
    src/runtimefile.cpp \
    src/service.cpp

HEADERS += \
    src/httpserver.h \
    src/storage.h \
    src/api.h \
    src/shares.h \
    src/config.h \
    src/runtimefile.h \
    src/service.h

# The service control manager.
win32: LIBS += -ladvapi32

unix {
    target.path = /usr/local/bin
    INSTALLS += target
}
