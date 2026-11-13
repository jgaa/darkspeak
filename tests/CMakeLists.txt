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
    QT_QUICK_CONTROLS_STYLE=org.kde.desktop
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

# Exercise the short alias before QApplication initializes settings/platform paths.
# A relative directory reproduces `darkspeak -p glad` without touching real profiles.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/short-profile-smoke")
add_test(NAME qml_smoke_short_profile COMMAND ${CMAKE_COMMAND} -E env
    QT_QPA_PLATFORM=offscreen
    QT_QUICK_CONTROLS_STYLE=org.kde.desktop
    XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/short-profile-smoke/fallback-config
    XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/short-profile-smoke/fallback-data
    XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/short-profile-smoke/fallback-cache
    DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE=darkspeak-short-profile-smoke-only
    $<TARGET_FILE:darkspeak>
    --qml-smoke-test
    -p glad
    --log-level error)
set_tests_properties(qml_smoke_short_profile PROPERTIES TIMEOUT 20
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/short-profile-smoke")

# Tor is an integration dependency, not a configure-time requirement. The
# workflow invokes this target only when a Tor daemon is available.
if(DS_ENABLE_TOR_TESTS)
    add_test(NAME tor_integration COMMAND ${CMAKE_COMMAND} -E env
        DARKSPEAK_TOR_INTEGRATION=1
        $<TARGET_FILE:crypto_handshake_test>)
    set_tests_properties(tor_integration PROPERTIES LABELS tor)
endif()

add_subdirectory(ui)

add_executable(ui_history_model_test ui_history_model_test.cpp)
target_compile_features(ui_history_model_test PRIVATE cxx_std_20)
target_link_libraries(ui_history_model_test PRIVATE models Qt6::Test)
add_test(NAME ui_history_model COMMAND ui_history_model_test)
set_tests_properties(ui_history_model PROPERTIES TIMEOUT 30)

add_executable(managed_storage_test managed_storage_test.cpp)
target_compile_features(managed_storage_test PRIVATE cxx_std_20)
target_link_libraries(managed_storage_test PRIVATE models prot Qt6::Test)
target_compile_definitions(managed_storage_test PRIVATE DS_STORAGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME managed_storage COMMAND managed_storage_test)
set_tests_properties(managed_storage PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(DARKSPEAK_WITH_MCP)
    add_executable(mcp_policy_test mcp_policy_test.cpp ../src/mcp/store.cpp)
    target_include_directories(mcp_policy_test PRIVATE ../src/mcp)
    target_compile_features(mcp_policy_test PRIVATE cxx_std_20)
    target_compile_definitions(mcp_policy_test PRIVATE MCP_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mcp")
    target_link_libraries(mcp_policy_test PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
    add_test(NAME mcp_policy COMMAND mcp_policy_test)
    add_executable(mcp_ui_test mcp_ui_test.cpp)
    target_compile_features(mcp_ui_test PRIVATE cxx_std_20)
    target_compile_definitions(mcp_ui_test PRIVATE MCP_UI_SOURCE="${CMAKE_SOURCE_DIR}/src/app")
    target_link_libraries(mcp_ui_test PRIVATE darkspeak_ui_test_support Qt6::Quick Qt6::QuickControls2 Qt6::Test)
    add_test(NAME mcp_ui COMMAND mcp_ui_test -nocrashhandler)
    set_tests_properties(mcp_ui PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    add_executable(mcp_service_test mcp_service_test.cpp)
    target_compile_features(mcp_service_test PRIVATE cxx_std_20)
    target_compile_definitions(mcp_service_test PRIVATE MCP_CLIENT_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/mcp/client_smoke.py")
    target_link_libraries(mcp_service_test PRIVATE darkspeak_mcp models prot Qt6::Test)
    target_include_directories(mcp_service_test PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/logfault/include)
    add_test(NAME mcp_service COMMAND mcp_service_test -nocrashhandler)
    set_tests_properties(mcp_policy mcp_service PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(DARKSPEAK_WITH_TEST_MCP)
    add_executable(mcp_test_instance mcp/test_instance.cpp)
    target_link_libraries(mcp_test_instance PRIVATE darkspeak_mcp)
    find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
    add_test(NAME mcp_harness COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/mcp/test_harness.py)
    set_tests_properties(mcp_harness PROPERTIES TIMEOUT 30 LABELS "mcp;integration")
    add_test(NAME mcp_bootstrap COMMAND ${CMAKE_COMMAND} -E env
        DARKSPEAK_TEST_INSTANCE=$<TARGET_FILE:mcp_test_instance>
        ${Python3_EXECUTABLE} -m pytest -q ${CMAKE_CURRENT_SOURCE_DIR}/mcp/test_bootstrap.py)
    set_tests_properties(mcp_bootstrap PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE LABELS "mcp;integration")
    add_test(NAME mcp_peer_happy_path COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/mcp/run_scenario.py
        --instance $<TARGET_FILE:mcp_test_instance> --chutney ${chutney_SOURCE_DIR}
        --artifacts ${CMAKE_CURRENT_BINARY_DIR}/mcp-artifacts)
    set_tests_properties(mcp_peer_happy_path PROPERTIES TIMEOUT 630 RUN_SERIAL TRUE LABELS "mcp;integration;tor")
endif()

if(DARKSPEAK_WITH_TEST_MCP)
    foreach(scenario privacy_multi resume_crash receiver_crash delete_contacts delete_sender file_decisions corrupt_partial image_previews image_previews_disabled video_previews video_previews_disabled)
        add_test(NAME mcp_peer_${scenario} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/mcp/run_scenario.py --scenario ${scenario}
            --instance $<TARGET_FILE:mcp_test_instance> --chutney ${chutney_SOURCE_DIR}
            --artifacts ${CMAKE_CURRENT_BINARY_DIR}/mcp-artifacts)
        set_tests_properties(mcp_peer_${scenario} PROPERTIES TIMEOUT 630 RUN_SERIAL TRUE LABELS "mcp;integration;tor")
    endforeach()
endif()

add_executable(database_recovery_test database_recovery_test.cpp)
target_compile_features(database_recovery_test PRIVATE cxx_std_20)
target_link_libraries(database_recovery_test PRIVATE core Qt6::Test)
add_test(NAME database_recovery COMMAND database_recovery_test)
set_tests_properties(database_recovery PROPERTIES TIMEOUT 30)

add_executable(media_preview_test media_preview_test.cpp ../src/corelib/src/mediapreview.cpp ../src/corelib/src/videopreview.cpp)
target_compile_features(media_preview_test PRIVATE cxx_std_20)
target_include_directories(media_preview_test PRIVATE ../src/corelib/include ../dependencies/logfault/include)
target_link_libraries(media_preview_test PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
target_compile_definitions(media_preview_test PRIVATE DS_VIDEO_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/media-preview-v1/blue.mkv")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(video_preview_probe video_preview_probe.cpp)
    target_compile_features(video_preview_probe PRIVATE cxx_std_20)
    target_compile_definitions(media_preview_test PRIVATE DS_VIDEO_PROBE="$<TARGET_FILE:video_preview_probe>")
    add_dependencies(media_preview_test video_preview_probe)
endif()
add_test(NAME media_preview COMMAND media_preview_test)
set_tests_properties(media_preview PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
