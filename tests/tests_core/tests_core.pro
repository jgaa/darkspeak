QT += testlib network core sql

CONFIG += qt console warn_on depend_includepath testcase
CONFIG -= app_bundle

TEMPLATE = app

SOURCES +=  \
    main.cpp \
    tst_dsengine.cpp

HEADERS += \
    tst_dsengine.h

INCLUDEPATH += \
    $$PWD/include \
    $$PWD/../../src/cryptolib/include \
    $$PWD/../../src/corelib/include \
    $$PWD/../../src/protlib/include


unix:!macx: LIBS += -lsodium

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../../src/corelib/release/ -lcorelib
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../../src/corelib/debug/ -lcorelib
else:unix: LIBS += -L$$OUT_PWD/../../src/corelib/ -lcorelib

INCLUDEPATH += $$PWD/../../src/corelib
DEPENDPATH += $$PWD/../../src/corelib

win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/corelib/release/libcorelib.a
else:win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/corelib/debug/libcorelib.a
else:win32:!win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/corelib/release/corelib.lib
else:win32:!win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/corelib/debug/corelib.lib
else:unix: PRE_TARGETDEPS += $$OUT_PWD/../../src/corelib/libcorelib.a

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../../src/protlib/release/ -lprotlib
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../../src/protlib/debug/ -lprotlib
else:unix: LIBS += -L$$OUT_PWD/../../src/protlib/ -lprotlib

INCLUDEPATH += $$PWD/../../src/protlib
DEPENDPATH += $$PWD/../../src/protlib

win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/protlib/release/libprotlib.a
else:win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/protlib/debug/libprotlib.a
else:win32:!win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/protlib/release/protlib.lib
else:win32:!win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/protlib/debug/protlib.lib
else:unix: PRE_TARGETDEPS += $$OUT_PWD/../../src/protlib/libprotlib.a

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
