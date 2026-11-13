add_library(darkspeak_mcp STATIC
    service.cpp service.h store.cpp store.h policy.h transport.cpp transport.h)
target_compile_features(darkspeak_mcp PUBLIC cxx_std_20)
target_include_directories(darkspeak_mcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/logfault/include)
target_compile_definitions(darkspeak_mcp PRIVATE DARKSPEAK_VERSION="${CMAKE_PROJECT_VERSION}")
target_link_libraries(darkspeak_mcp PUBLIC core PRIVATE yahat-cpp::yahat-cpp)
if(DARKSPEAK_WITH_TEST_MCP)
    target_sources(darkspeak_mcp PRIVATE test_support.cpp test_support.h test_runtime.cpp test_runtime.h)
    target_compile_definitions(darkspeak_mcp PUBLIC DARKSPEAK_WITH_TEST_MCP=1)
    target_link_libraries(darkspeak_mcp PRIVATE models prot Qt6::Widgets)
endif()
