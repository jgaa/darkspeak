
TEMPLATE = subdirs

SUBDIRS = \
    torlib \
    dscorelib \
    dsprotlib \
    cryptolib \
    test_tor \
    test_crypto

torlib.subdir = src/torlib
dscorelib.subdir = src/dscorelib
dsprotlib.subdir = src/dsprotlib
cryptolib.subdir = src/cryptolib

dsprotlib.depends = torlib
dscorelib.depends = dsprotlib cryptolib

test_tor.subdir = tests/tor_tests
test_tor.depends = torlib

test_crypto.subdir = tests/crypto_tests
test_crypto.depends = cryptolib
