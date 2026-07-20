cmake_minimum_required(VERSION 3.20)

project(mirage_display_qml LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Qml Quick)

if(COMMAND qt_policy)
    qt_policy(SET QTP0001 NEW)
endif()

set(MIRAGE_DISPLAY_QML_URI "Mirage.Display"
    CACHE STRING "QML module URI for the Mirage display item")
string(REPLACE "." "/" MIRAGE_DISPLAY_QML_PATH "${MIRAGE_DISPLAY_QML_URI}")

qt_add_qml_module(mirage-display-qml
    URI ${MIRAGE_DISPLAY_QML_URI}
    VERSION 1.0
    PLUGIN_TARGET mirage-display-qml
    SOURCES
        MirageDisplayItem.hpp
        MirageDisplayItem.cpp
    OUTPUT_DIRECTORY
        ${CMAKE_CURRENT_BINARY_DIR}/${MIRAGE_DISPLAY_QML_PATH})

target_link_libraries(mirage-display-qml PRIVATE
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    mirage_display_egl_static)
target_compile_options(mirage-display-qml PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
target_link_options(mirage-display-qml PRIVATE -Wl,--as-needed)

set(MIRAGE_DISPLAY_QML_INSTALL_DIR
    "${CMAKE_INSTALL_FULL_LIBDIR}/qt6/qml"
    CACHE PATH "Qt 6 QML module install directory")
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${MIRAGE_DISPLAY_QML_PATH}/
    DESTINATION ${MIRAGE_DISPLAY_QML_INSTALL_DIR}/${MIRAGE_DISPLAY_QML_PATH})
