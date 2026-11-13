cmake_minimum_required(VERSION 3.24)
project (ds VERSION 0.3.0 LANGUAGES CXX)

string(TIMESTAMP DARKSPEAK_BUILD_DATE "%Y-%m-%d %H:%M:%S UTC" UTC)
set(DARKSPEAK_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

option(DARKSPEAK_FLATPAK_BUILD "Build the Flatpak package" OFF)

include(cmake/FindSodium.cmake)
include(FetchContent)

option(DARKSPEAK_WITH_MCP "Enable MCP integration" OFF)
option(DARKSPEAK_WITH_TEST_MCP "Enable MCP test extensions" OFF)
# CI toolchain selections; local tests support Python 3.12+ and the normal Qt minimum.
set(DARKSPEAK_TEST_QT_VERSION 6.10.2)
set(DARKSPEAK_TEST_PYTHON_VERSION 3.14)
if(DARKSPEAK_WITH_TEST_MCP AND NOT DARKSPEAK_WITH_MCP)
    message(FATAL_ERROR "DARKSPEAK_WITH_TEST_MCP requires DARKSPEAK_WITH_MCP")
endif()
# Test infrastructure is fetched only for explicitly enabled test builds.
if(DARKSPEAK_WITH_TEST_MCP)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The isolated MCP test bootstrap currently requires Linux")
    endif()
    set(DARKSPEAK_CHUTNEY_REVISION 4bfd74bfb1f7b1bc663318c47fc8119668fffca3)
    FetchContent_Declare(chutney
        GIT_REPOSITORY https://gitlab.torproject.org/tpo/core/chutney.git
        GIT_TAG ${DARKSPEAK_CHUTNEY_REVISION}
        GIT_SHALLOW FALSE)
    FetchContent_GetProperties(chutney)
    if(NOT chutney_POPULATED)
        FetchContent_Populate(chutney)
    endif()
endif()
if(DARKSPEAK_WITH_MCP)
    set(YAHAT_ENABLE_MCP ON CACHE BOOL "" FORCE)
    set(YAHAT_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(YAHAT_WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(YAHAT_ENABLE_METRICS OFF CACHE BOOL "" FORCE)
    set(DARKSPEAK_YAHAT_REVISION 67cb6c31c13f695fc548c71f7789a0619b1333d4)
    FetchContent_Declare(yahat
        GIT_REPOSITORY https://github.com/jgaa/yahat-cpp.git
        GIT_TAG ${DARKSPEAK_YAHAT_REVISION}
        GIT_SHALLOW FALSE)
    # Populate first so offline FetchContent source preparation follows exactly
    # the same patch path. No installed package or sibling source discovery.
    FetchContent_GetProperties(yahat)
    if(NOT yahat_POPULATED)
        FetchContent_Populate(yahat)
    endif()
    set(SOURCE_DIR "${yahat_SOURCE_DIR}")
    set(PATCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/yahat-hardening.patch")
    include(cmake/PatchYahat.cmake)
    add_subdirectory("${yahat_SOURCE_DIR}" "${yahat_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()


# Secret material is kept out of the DarkSpeak database.  SafeKeeping is
# fetched by CMake so release builds do not depend on a manually installed
# copy, while the exact revision remains pinned for reproducibility.
FetchContent_Declare(safekeeping
    GIT_REPOSITORY https://github.com/jgaa/SafeKeeping.git
    GIT_TAG c53b3f5ad5ec150058e0313a856aa3ad2c68f9e1
    # A shallow HEAD clone cannot check out this historical pinned commit.
    GIT_SHALLOW FALSE)
set(SAFEKEEPING_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SAFEKEEPING_ENABLE_KWALLET OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(safekeeping)

# Nayuki's generator is fetched at configure time.  Keep the source outside
# the repository and build only the small C++ implementation used by the app.
FetchContent_Declare(nayuki_qr_generator
    GIT_REPOSITORY https://github.com/nayuki/QR-Code-generator.git
    GIT_TAG v1.8.0
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(nayuki_qr_generator)
if(NOT nayuki_qr_generator_POPULATED)
    FetchContent_Populate(nayuki_qr_generator)
endif()

add_library(nayuki_qr_generator STATIC
    ${nayuki_qr_generator_SOURCE_DIR}/cpp/qrcodegen.cpp)
target_include_directories(nayuki_qr_generator PUBLIC
    ${nayuki_qr_generator_SOURCE_DIR}/cpp)
target_compile_features(nayuki_qr_generator PRIVATE cxx_std_17)


if(NOT ANDROID)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif()

if (sodium_INCLUDE_DIR MATCHES "NOTFOUND")
    message(FATAL_ERROR "Failed to locate required library libsodium")
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.10 COMPONENTS Core Gui Qml Quick QuickControls2 LinguistTools Protobuf Widgets Network Sql Svg Test REQUIRED)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 6.10 COMPONENTS DBus REQUIRED)
endif()
include(cmake/KdeColorScheme.cmake)

add_subdirectory(src/cryptolib)
option(DSFM_BUILD_TESTS "Build standalone file manager tests" OFF)
option(DSFM_BUILD_EXAMPLE "Build the standalone file manager example" OFF)
add_subdirectory(src/filemanager)
add_subdirectory(src/corelib)
add_subdirectory(src/torlib)
add_subdirectory(src/protlib)
add_subdirectory(src/modelslib)
if(DARKSPEAK_WITH_MCP)
    target_compile_definitions(core PUBLIC DARKSPEAK_WITH_MCP=1)
    add_subdirectory(src/mcp)
endif()
add_subdirectory(src/app)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
