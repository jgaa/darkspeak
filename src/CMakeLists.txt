
add_subdirectory(lib)
add_subdirectory(md5-cc)

if (WITH_QT_QUICK_GUI)
    message(STATUS "Building with QT Quick GUI")
    add_subdirectory(qt-quick-gui)
endif()
