#-------------------------------------------------
#
# Project created by QtCreator 2018-03-05T11:38:46
#
#-------------------------------------------------

QT       += core sql gui network
INCLUDEPATH += $$PWD/../../dependencies/logfault/include/
TARGET = modelslib
TEMPLATE = lib
CONFIG += staticlib
DEFINES += QT_DEPRECATED_WARNINGS LOGFAULT_ENABLE_LOCATION=1

macx {
    INCLUDEPATH += /usr/local/Cellar/libsodium/1.0.17/include
}

unix {
    CONFIG += c++14
}


SOURCES += \
    src/contactsmodel.cpp \
    src/conversationsmodel.cpp \
    src/manager.cpp \
    src/logmodel.cpp \
    src/notificationsmodel.cpp \
    src/messagesmodel.cpp \
    src/filesmodel.cpp

HEADERS += \
    include/ds/contactsmodel.h \
    include/ds/strategy.h \
    include/ds/model_util.h \
    include/ds/conversationsmodel.h \
    include/ds/manager.h \
    include/ds/logmodel.h \
    include/ds/notificationsmodel.h \
    include/ds/messagesmodel.h \
    include/ds/filesmodel.h

INCLUDEPATH += \
    $$PWD/include \
    $$PWD/../corelib/include \
    $$PWD/../cryptolib/include \
    $$PWD/../protlib/include

