#-------------------------------------------------
#
# Project created by QtCreator 2018-02-27T16:15:44
#
#-------------------------------------------------

QT       += core sql network
QT       -= gui
TARGET = corelib
TEMPLATE = lib
CONFIG += staticlib
DEFINES += LOGFAULT_ENABLE_LOCATION=1

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    src/dsengine.cpp \
    src/database.cpp \
    src/protocolmanager.cpp \
    src/message.cpp \
    src/identitymanager.cpp \
    src/identity.cpp \
    src/contact.cpp \
    src/contactmanager.cpp \
    src/conversation.cpp \
    src/conversationmanager.cpp \
    src/messagemanager.cpp

HEADERS += \
    include/ds/dsengine.h \
    include/ds/protocolmanager.h \
    include/ds/database.h \
    include/ds/task.h \
    include/ds/identity.h \
    include/ds/errors.h \
    include/ds/transporthandle.h \
    include/ds/contact.h \
    include/ds/message.h \
    include/ds/update_helper.h \
    include/ds/identitymanager.h \
    include/ds/contactmanager.h \
    include/ds/registry.h \
    include/ds/peerconnection.h \
    include/ds/conversation.h \
    include/ds/conversationmanager.h \
    include/ds/lru_cache.h \
    include/ds/messagemanager.h

INCLUDEPATH += \
    $$PWD/../../dependencies/logfault/include \
    $$PWD/include \
    $$PWD/../cryptolib/include \
    $$PWD/../protlib/include \
    $$PWD/../torlib/include \
