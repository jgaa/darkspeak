
TEMPLATE = subdirs

SUBDIRS = \
    torlib \
    corelib \
    protlib \
    cryptolib \
    modelslib \
    test_tor \
    test_crypto \
    test_core \
    test_models

torlib.subdir = src/torlib
corelib.subdir = src/corelib
protlib.subdir = src/protlib
cryptolib.subdir = src/cryptolib
modelslib.subdir = src/modelslib

modelslib.depends = corelib

protlib.depends = torlib
corelib.depends = torlib protlib cryptolib

test_core.subdir = tests/tests_core
test_core.depends = corelib torlib cryptolib protlib

test_tor.subdir = tests/tests_tor
test_tor.depends = torlib cryptolib

test_crypto.subdir = tests/tests_crypto
test_crypto.depends = cryptolib

test_models.subdir = tests/tests_models
test_models.depends = modelslib
