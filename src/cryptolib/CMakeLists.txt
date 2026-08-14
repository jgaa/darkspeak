project(crypto LANGUAGES CXX)

add_library(${PROJECT_NAME} STATIC
    include/ds/memoryview.h
    include/ds/cvar.h
    include/ds/crypto.h
    include/ds/dscert.h
    include/ds/certimpl.h
    include/ds/safememory.h
    include/ds/base32.h
    include/ds/base58.h
    src/crypto.cpp
    src/certimpl.cpp
    src/base32.cpp
    src/base58.cpp
    )
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
#add_dependencies(${PROJECT_NAME} corelib)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/logfault/include)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)
target_link_libraries(${PROJECT_NAME} PUBLIC Qt6::Core Qt6::Network sodium)
