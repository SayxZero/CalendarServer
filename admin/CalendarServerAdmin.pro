# qmake build of the admin panel; CMakeLists.txt describes the same program.
# It compiles the server's own sources too: it reads the same base the same way.
QT = core network sql widgets
CONFIG += c++17
CONFIG -= app_bundle

TARGET = CalendarServerAdmin
VERSION = 1.1.1
DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000 SERVER_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += ../src .

SOURCES += \
    main.cpp \
    admindb.cpp \
    adminsession.cpp \
    backuppage.cpp \
    backups.cpp \
    chart.cpp \
    logpage.cpp \
    look.cpp \
    mainwindow.cpp \
    page.cpp \
    records.cpp \
    serverpage.cpp \
    servicecontrol.cpp \
    statspage.cpp \
    userspage.cpp \
    ../src/httpserver.cpp \
    ../src/storage.cpp \
    ../src/api.cpp \
    ../src/shares.cpp \
    ../src/config.cpp \
    ../src/service.cpp

HEADERS += \
    admindb.h \
    adminsession.h \
    backuppage.h \
    backups.h \
    chart.h \
    logpage.h \
    look.h \
    mainwindow.h \
    page.h \
    records.h \
    serverpage.h \
    servicecontrol.h \
    statspage.h \
    userspage.h \
    ../src/httpserver.h \
    ../src/storage.h \
    ../src/api.h \
    ../src/shares.h \
    ../src/config.h \
    ../src/service.h

win32 {
    # The icon and the name Explorer shows; the service control manager and
    # the UAC question.
    RC_FILE = admin.rc
    LIBS += -ladvapi32 -lshell32
}

unix {
    target.path = /usr/local/bin
    INSTALLS += target
}
