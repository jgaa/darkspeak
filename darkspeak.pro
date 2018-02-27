
TEMPLATE = subdirs

SUBDIRS = \
    torlib \
    test_tor \
    dscorelib \
    src/dsprotlib

torlib.subdir = src/torlib
dscorelib.subdir = src/dscorelib

dscorelib.depends = torlib

test_tor.subdir = tests/tor_tests
test_tor.depends = torlib

