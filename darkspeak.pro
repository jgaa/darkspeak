
TEMPLATE = subdirs

SUBDIRS = \
    torlib \
    test_tor

torlib.subdir = src/torlib

test_tor.subdir = tests/tor_tests
test_tor.depends = torlib

