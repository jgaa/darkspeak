project(tests)

add_executable(devel_tests devel_tests.cpp)
target_link_libraries(devel_tests
    darkspeak_api
    ${DEFAULT_LIBRARIES}
)

add_test(TMP_DEVELOPMENT_TESTS devel_tests)

