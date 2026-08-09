cmake_minimum_required(VERSION 3.16)
project (ds VERSION 0.2.1 LANGUAGES CXX)

option(DARKSPEAK_FLATPAK_BUILD "Build the Flatpak package" OFF)

include(cmake/FindSodium.cmake)
include(FetchContent)

# Secret material is kept out of the DarkSpeak database.  SafeKeeping is
# fetched by CMake so release builds do not depend on a manually installed
# copy, while the exact revision remains pinned for reproducibility.
FetchContent_Declare(safekeeping
    GIT_REPOSITORY https://github.com/jgaa/SafeKeeping.git
    GIT_TAG c53b3f5ad5ec150058e0313a856aa3ad2c68f9e1
    GIT_SHALLOW TRUE)
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

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.10 COMPONENTS Core Gui Qml Quick QuickControls2 LinguistTools Widgets Network Sql Svg Test REQUIRED)

add_subdirectory(src/cryptolib)
add_subdirectory(src/corelib)
add_subdirectory(src/torlib)
add_subdirectory(src/protlib)
add_subdirectory(src/modelslib)
add_subdirectory(src/app)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
