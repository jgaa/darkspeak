QT += testlib network
QT -= gui

CONFIG += qt console warn_on depend_includepath testcase
CONFIG -= app_bundle

TEMPLATE = app

INCLUDEPATH += $$PWD/../../src/cryptolib/include

SOURCES +=  \
    main.cpp \
    tst_certs.cpp

INCLUDEPATH += $$PWD/../../src/cryptolib
DEPENDPATH += $$PWD/../../src/cryptolib


HEADERS += \
    tst_certs.h

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../../src/cryptolib/release/ -lcryptolib
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../../src/cryptolib/debug/ -lcryptolib
else:unix: LIBS += -L$$OUT_PWD/../../src/cryptolib/ -lcryptolib

INCLUDEPATH += $$PWD/../../src/cryptolib
DEPENDPATH += $$PWD/../../src/cryptolib

win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/cryptolib/release/libcryptolib.a
else:win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/cryptolib/debug/libcryptolib.a
else:win32:!win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/cryptolib/release/cryptolib.lib
else:win32:!win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/cryptolib/debug/cryptolib.lib
else:unix: PRE_TARGETDEPS += $$OUT_PWD/../../src/cryptolib/libcryptolib.a

LIBS += -lsodium
