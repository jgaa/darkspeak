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
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/logfault/include)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)
target_link_libraries(${PROJECT_NAME} PUBLIC core crypto tor Qt6::Core Qt6::Gui Qt6::Network Qt6::Quick Qt6::Sql Qt6::Svg Qt6::Widgets)
