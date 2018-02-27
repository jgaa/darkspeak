
TEMPLATE = subdirs

SUBDIRS = \
    torlib \
    test_tor \
    dscorelib \
    dsprotlib

torlib.subdir = src/torlib
dscorelib.subdir = src/dscorelib
dsprotlib.subdir = src/dsprotlib

dsprotlib.depends-= torlib
dscorelib.depends = dsprotlib

test_tor.subdir = tests/tor_tests
test_tor.depends = torlib

