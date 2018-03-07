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
    $$PWD/../../src/dscorelib/include \
    $$PWD/../../src/dsprotlib/include


unix:!macx: LIBS += -lcrypto



win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../../src/dscorelib/release/ -ldscorelib
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../../src/dscorelib/debug/ -ldscorelib
else:unix: LIBS += -L$$OUT_PWD/../../src/dscorelib/ -ldscorelib

INCLUDEPATH += $$PWD/../../src/dscorelib
DEPENDPATH += $$PWD/../../src/dscorelib

win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/dscorelib/release/libdscorelib.a
else:win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/dscorelib/debug/libdscorelib.a
else:win32:!win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/dscorelib/release/dscorelib.lib
else:win32:!win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/dscorelib/debug/dscorelib.lib
else:unix: PRE_TARGETDEPS += $$OUT_PWD/../../src/dscorelib/libdscorelib.a

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../../src/dsprotlib/release/ -ldsprotlib
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../../src/dsprotlib/debug/ -ldsprotlib
else:unix: LIBS += -L$$OUT_PWD/../../src/dsprotlib/ -ldsprotlib

INCLUDEPATH += $$PWD/../../src/dsprotlib
DEPENDPATH += $$PWD/../../src/dsprotlib

win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/dsprotlib/release/libdsprotlib.a
else:win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/dsprotlib/debug/libdsprotlib.a
else:win32:!win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/dsprotlib/release/dsprotlib.lib
else:win32:!win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/dsprotlib/debug/dsprotlib.lib
else:unix: PRE_TARGETDEPS += $$OUT_PWD/../../src/dsprotlib/libdsprotlib.a

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../../src/torlib/release/ -ltor
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../../src/torlib/debug/ -ltor
else:unix: LIBS += -L$$OUT_PWD/../../src/torlib/ -ltor

INCLUDEPATH += $$PWD/../../src/torlib
DEPENDPATH += $$PWD/../../src/torlib

win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/torlib/release/libtor.a
else:win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/torlib/debug/libtor.a
else:win32:!win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/torlib/release/tor.lib
else:win32:!win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../../src/torlib/debug/tor.lib
else:unix: PRE_TARGETDEPS += $$OUT_PWD/../../src/torlib/libtor.a

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
