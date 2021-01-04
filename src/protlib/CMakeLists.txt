project(prot LANGUAGES CXX)

add_library(${PROJECT_NAME} STATIC
    src/torsocketlistener.cpp
    src/torserviceinterface.cpp
    src/dsclient.cpp
    src/protmanager.cpp
    src/torprotocolmanager.cpp
    src/dsserver.cpp
    src/imageutil.cpp
    src/connectionsocket.cpp
    src/peer.cpp
    include/ds/dsserver.h
    include/ds/protmanager.h
    include/ds/peer.h
    include/ds/connectionsocket.h
    include/ds/imageutil.h
    include/ds/torprotocolmanager.h
    include/ds/dsclient.h
    include/ds/torsocketlistener.h
    include/ds/torserviceinterface.h
)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
add_dependencies(${PROJECT_NAME} core crypto)
target_link_libraries(${PROJECT_NAME} PRIVATE Qt5::Quick Qt5::Widgets Qt5::Core Qt5::Network Qt5::Sql Qt5::Svg)
