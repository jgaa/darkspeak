project(models LANGUAGES CXX)

add_library(${PROJECT_NAME} STATIC
    include/ds/notificationsmodel.h
    include/ds/logmodel.h
    include/ds/contactsmodel.h
    include/ds/model_util.h
    include/ds/messagesmodel.h
    include/ds/filesmodel.h
    include/ds/strategy.h
    include/ds/imageprovider.h
    include/ds/manager.h
    include/ds/conversationsmodel.h
    include/ds/identitynamevalidator.h
    src/messagesmodel.cpp
    src/conversationsmodel.cpp
    src/imageprovider.cpp
    src/identitynamevalidator.cpp
    src/logmodel.cpp
    src/contactsmodel.cpp
    src/manager.cpp
    src/filesmodel.cpp
    src/notificationsmodel.cpp
    )
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/logfault/include)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)
target_compile_definitions(${PROJECT_NAME} PUBLIC
    PROGRAM_VERSION="${CMAKE_PROJECT_VERSION}"
    DARKSPEAK_COMPILER="${DARKSPEAK_COMPILER}"
    DARKSPEAK_BUILD_DATE="${DARKSPEAK_BUILD_DATE}")
target_link_libraries(${PROJECT_NAME} PUBLIC prot tor core crypto nayuki_qr_generator
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Quick Qt6::Sql Qt6::Svg Qt6::Widgets)
