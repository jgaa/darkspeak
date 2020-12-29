project(crypto LANGUAGES CXX)

add_library(${PROJECT_NAME} STATIC
    include/ds/memoryview.h
    include/ds/cvar.h
    include/ds/crypto.h
    include/ds/dscert.h
    include/ds/certimpl.h
    #include/ds/rsacertimpl.h
    include/ds/safememory.h
    include/ds/base32.h
    include/ds/base58.h
    src/crypto.cpp
    #src/rsacertimpl.cpp
    src/certimpl.cpp
    src/base32.cpp
    src/base58.cpp
    )
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
#add_dependencies(${PROJECT_NAME} corelib)
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Network ${sodium_LIBRARY})
