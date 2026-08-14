add_executable(crypto_handshake_test
    crypto_handshake_test.cpp
)
target_compile_features(crypto_handshake_test PRIVATE cxx_std_20)
target_link_libraries(crypto_handshake_test PRIVATE crypto Qt6::Core Qt6::Test sodium)
add_test(NAME crypto_handshake COMMAND crypto_handshake_test)

add_executable(protocol_v3_test
    protocol_v3_test.cpp
)
target_compile_features(protocol_v3_test PRIVATE cxx_std_17)
target_link_libraries(protocol_v3_test PRIVATE prot core Qt6::Core Qt6::Test)
add_test(NAME protocol_v3 COMMAND protocol_v3_test)
set_tests_properties(protocol_v3 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(tor_address_test tor_address_test.cpp)
target_compile_features(tor_address_test PRIVATE cxx_std_17)
target_link_libraries(tor_address_test PRIVATE tor crypto Qt6::Core Qt6::Test sodium)
add_test(NAME tor_address COMMAND tor_address_test)
set_tests_properties(tor_address PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(tor_config_test tor_config_test.cpp)
target_compile_features(tor_config_test PRIVATE cxx_std_17)
target_link_libraries(tor_config_test PRIVATE tor Qt6::Core Qt6::Test)
add_test(NAME tor_config COMMAND tor_config_test)
set_tests_properties(tor_config PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_test(NAME qml_smoke COMMAND ${CMAKE_COMMAND} -E env
    QT_QPA_PLATFORM=offscreen
    QT_QUICK_CONTROLS_STYLE=Fusion
    XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/qml-smoke-config
    XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/qml-smoke-data
    XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/qml-smoke-cache
    DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE=darkspeak-qml-smoke-only
    $<TARGET_FILE:darkspeak>
    --qml-smoke-test
    --profile ${CMAKE_CURRENT_BINARY_DIR}/qml-smoke-profile
    --log-file ${CMAKE_CURRENT_BINARY_DIR}/qml-smoke.log
    --log-level error)
set_tests_properties(qml_smoke PROPERTIES TIMEOUT 20)

# Tor is an integration dependency, not a configure-time requirement. The
# workflow invokes this target only when a Tor daemon is available.
if(DS_ENABLE_TOR_TESTS)
    add_test(NAME tor_integration COMMAND ${CMAKE_COMMAND} -E env
        DARKSPEAK_TOR_INTEGRATION=1
        $<TARGET_FILE:crypto_handshake_test>)
    set_tests_properties(tor_integration PROPERTIES LABELS tor)
endif()
