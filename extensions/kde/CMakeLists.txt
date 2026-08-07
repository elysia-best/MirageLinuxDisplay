cmake_minimum_required(VERSION 3.20)

project(mirage_wallpaper VERSION 0.1.0 LANGUAGES C CXX)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/stage" CACHE PATH
        "KDE package staging prefix" FORCE)
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "KDE package build type" FORCE)
endif()

set(MIRAGE_DISPLAY_ENABLE_INSTALL OFF CACHE BOOL
    "Disable the core library install rules for the KDE package" FORCE)
set(MIRAGE_DISPLAY_KDE_PACKAGE ON CACHE BOOL
    "Build the self-contained KDE wallpaper package" FORCE)
set(MIRAGE_DISPLAY_PLUGIN_QML ON CACHE BOOL
    "Build the Plasma Qt Quick display adapter" FORCE)
set(MIRAGE_DISPLAY_QML_URI "Mirage.DisplayEmbed" CACHE STRING
    "Embed the QML module in the wallpaper package" FORCE)
set(MIRAGE_DISPLAY_WITH_EGL ON CACHE BOOL
    "Build the EGL DMA-BUF import helper" FORCE)
set(MIRAGE_DISPLAY_BUILD_TESTS OFF CACHE BOOL
    "Do not build the core test suite for the KDE package" FORCE)
set(MIRAGE_DISPLAY_BUILD_EXAMPLES OFF CACHE BOOL
    "Do not build the core examples for the KDE package" FORCE)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_BINARY_DIR}/mirage-display")
