project(tor LANGUAGES CXX)

add_library(${PROJECT_NAME} STATIC
    src/torctlsocket.cpp
    src/torcontroller.cpp
    src/tormgr.cpp
    include/ds/serviceproperties.h
    include/ds/torctlsocket.h
    include/ds/tormgr.h
    include/ds/torconfig.h
    include/ds/torcontroller.h
    )
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
add_dependencies(${PROJECT_NAME} core)
target_link_libraries(${PROJECT_NAME} PRIVATE Qt5::Core Qt5::Network)
