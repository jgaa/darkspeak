QT += testlib network
QT -= gui

CONFIG += qt console warn_on depend_includepath testcase
CONFIG -= app_bundle

TEMPLATE = app

INCLUDEPATH += $$PWD/../../src/torlib/include

SOURCES +=  \
    tst_tormanager.cpp \
    tst_torctlsocket.cpp \
    main.cpp \
    tst_torcontroller.cpp

HEADERS += \
    tst_torctlsocket.h \
    tst_tormanager.h \
    tst_torcontroller.h


unix:!macx: LIBS += -lsodium

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../../src/torlib/release/ -ltorlib
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../../src/torlib/debug/ -ltorlib
else:unix: LIBS += -L$$OUT_PWD/../../src/torlib/ -ltorlib

INCLUDEPATH += $$PWD/../../src/torlib
DEPENDPATH += $$PWD/../../src/torlib

win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/torlib/release/libtorlib.a
else:win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/torlib/debug/libtorlib.a
else:win32:!win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/torlib/release/torlib.lib
else:win32:!win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/torlib/debug/torlib.lib
else:unix: PRE_TARGETDEPS += $$OUT_PWD/../../src/torlib/libtorlib.a


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
