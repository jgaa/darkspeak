
TEMPLATE = subdirs

SUBDIRS = \
    torlib \
    dscorelib \
    dsprotlib \
    cryptolib \
    modelslib \
    test_tor \
    test_crypto \
    test_core \
    test_models

torlib.subdir = src/torlib
dscorelib.subdir = src/dscorelib
dsprotlib.subdir = src/dsprotlib
cryptolib.subdir = src/cryptolib
modelslib.subdir = src/modelslib

modelslib.depends = dscorelib

dsprotlib.depends = torlib
dscorelib.depends = dsprotlib cryptolib

test_core.subdir = tests/tests_core
test_core.depends = dscorelib torlib cryptolib dsprotlib

test_tor.subdir = tests/tests_tor
test_tor.depends = torlib cryptolib

test_crypto.subdir = tests/tests_crypto
test_crypto.depends = cryptolib

test_models.subdir = tests/tests_models
test_crypto.depends = modelslib dscorelib torlib cryptolib dsprotlib
