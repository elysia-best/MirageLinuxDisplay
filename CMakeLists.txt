cmake_minimum_required(VERSION 3.20)

project(mirage_linux_display
    VERSION 0.1.0
    DESCRIPTION "Desktop-environment display integration for MirageWallpaper"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

set(MIRAGE_DISPLAY_SOURCES
    src/codec.c
    src/protocol.c
    src/display.c
    src/producer.c
    src/broker.c
    src/sync.c)

add_library(mirage_display SHARED ${MIRAGE_DISPLAY_SOURCES})
add_library(mirage_display_static STATIC ${MIRAGE_DISPLAY_SOURCES})
set_target_properties(mirage_display_static PROPERTIES OUTPUT_NAME mirage_display)

foreach(target mirage_display mirage_display_static)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endforeach()

set_target_properties(mirage_display PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

install(TARGETS mirage_display mirage_display_static
    EXPORT MirageDisplayTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/mirage_display.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES include/mirage_display_producer.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES include/mirage_display_broker.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT MirageDisplayTargets
    FILE MirageDisplayTargets.cmake
    NAMESPACE Mirage::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MirageDisplay)

find_package(Vulkan QUIET)
option(MIRAGE_DISPLAY_WITH_VULKAN "Build the Vulkan DMA-BUF import helper" ${Vulkan_FOUND})
if(MIRAGE_DISPLAY_WITH_VULKAN)
    if(NOT Vulkan_FOUND)
        message(FATAL_ERROR "MIRAGE_DISPLAY_WITH_VULKAN=ON but Vulkan was not found")
    endif()
    add_library(mirage_display_vulkan SHARED
        src/vulkan_import.c
        src/vulkan_blit.c
        src/vulkan_export.c)
    add_library(mirage_display_vulkan_static STATIC
        src/vulkan_import.c
        src/vulkan_blit.c
        src/vulkan_export.c)
    set_target_properties(mirage_display_vulkan PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
    set_target_properties(mirage_display_vulkan_static PROPERTIES
        OUTPUT_NAME mirage_display_vulkan)
    foreach(target mirage_display_vulkan mirage_display_vulkan_static)
        target_include_directories(${target}
            PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endforeach()
    target_link_libraries(mirage_display_vulkan PUBLIC mirage_display Vulkan::Vulkan)
    target_link_libraries(mirage_display_vulkan_static PUBLIC mirage_display_static Vulkan::Vulkan)
    install(TARGETS mirage_display_vulkan mirage_display_vulkan_static
        EXPORT MirageDisplayTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES
        include/mirage_display_vulkan.h
        include/mirage_display_vulkan_blit.h
        include/mirage_display_vulkan_export.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(EGL QUIET IMPORTED_TARGET egl)
    pkg_check_modules(GLESV2 QUIET IMPORTED_TARGET glesv2)
endif()
set(_mirage_egl_default OFF)
if(EGL_FOUND AND GLESV2_FOUND)
    set(_mirage_egl_default ON)
endif()
option(MIRAGE_DISPLAY_WITH_EGL "Build the EGL DMA-BUF import helper" ${_mirage_egl_default})
if(MIRAGE_DISPLAY_WITH_EGL)
    if(NOT EGL_FOUND OR NOT GLESV2_FOUND)
        message(FATAL_ERROR "MIRAGE_DISPLAY_WITH_EGL=ON but EGL/GLESv2 were not found")
    endif()
    add_library(mirage_display_egl SHARED src/egl_import.c)
    add_library(mirage_display_egl_static STATIC src/egl_import.c)
    set_target_properties(mirage_display_egl PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
    set_target_properties(mirage_display_egl_static PROPERTIES OUTPUT_NAME mirage_display_egl)
    foreach(target mirage_display_egl mirage_display_egl_static)
        target_include_directories(${target}
            PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endforeach()
    target_link_libraries(mirage_display_egl PUBLIC
        mirage_display PkgConfig::EGL PkgConfig::GLESV2)
    target_link_libraries(mirage_display_egl_static PUBLIC
        mirage_display_static PkgConfig::EGL PkgConfig::GLESV2)
    install(TARGETS mirage_display_egl mirage_display_egl_static
        EXPORT MirageDisplayTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES include/mirage_display_egl.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

option(MIRAGE_DISPLAY_BUILD_TESTS "Build protocol and session tests" ON)
if(MIRAGE_DISPLAY_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)

    add_executable(test_codec tests/test_codec.c)
    target_link_libraries(test_codec PRIVATE mirage_display_static)
    target_include_directories(test_codec PRIVATE src)
    target_compile_options(test_codec PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME codec COMMAND test_codec)

    add_executable(test_protocol tests/test_protocol.c)
    target_link_libraries(test_protocol PRIVATE mirage_display_static)
    target_include_directories(test_protocol PRIVATE src)
    target_compile_options(test_protocol PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME protocol COMMAND test_protocol)

    add_executable(test_session tests/test_session.c)
    target_link_libraries(test_session PRIVATE mirage_display_static Threads::Threads)
    target_include_directories(test_session PRIVATE src)
    target_compile_options(test_session PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME session COMMAND test_session)

    add_executable(test_broker tests/test_broker.c)
    target_link_libraries(test_broker PRIVATE mirage_display_static Threads::Threads)
    target_include_directories(test_broker PRIVATE src)
    target_compile_options(test_broker PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME broker COMMAND test_broker)
    set_tests_properties(broker PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(test_producer tests/test_producer.c)
    target_link_libraries(test_producer PRIVATE mirage_display_static Threads::Threads)
    target_include_directories(test_producer PRIVATE src)
    target_compile_options(test_producer PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME producer COMMAND test_producer)

    add_executable(test_sync tests/test_sync.c)
    target_link_libraries(test_sync PRIVATE mirage_display_static)
    target_include_directories(test_sync PRIVATE src)
    target_compile_options(test_sync PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME sync COMMAND test_sync)

    if(MIRAGE_DISPLAY_WITH_VULKAN)
        add_executable(test_vulkan tests/test_vulkan.c)
        target_link_libraries(test_vulkan PRIVATE mirage_display_vulkan_static)
        target_compile_options(test_vulkan PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME vulkan COMMAND test_vulkan)
    endif()

    if(MIRAGE_DISPLAY_WITH_EGL)
        add_executable(test_egl tests/test_egl.c)
        target_link_libraries(test_egl PRIVATE mirage_display_egl_static)
        target_compile_options(test_egl PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME egl COMMAND test_egl)
    endif()
endif()

option(MIRAGE_DISPLAY_BUILD_EXAMPLES "Build protocol simulator examples" ON)
if(MIRAGE_DISPLAY_BUILD_EXAMPLES)
    add_executable(mirage_headless_consumer examples/headless_consumer.c)
    target_link_libraries(mirage_headless_consumer PRIVATE mirage_display_static)
    target_compile_options(mirage_headless_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)

    add_executable(mirage_mock_broker examples/mock_broker.c)
    target_link_libraries(mirage_mock_broker PRIVATE mirage_display_static)
    target_include_directories(mirage_mock_broker PRIVATE src)
    target_compile_options(mirage_mock_broker PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

option(MIRAGE_DISPLAY_PLUGIN_QML "Build the Plasma Qt Quick display adapter" OFF)
if(MIRAGE_DISPLAY_PLUGIN_QML)
    if(NOT MIRAGE_DISPLAY_WITH_EGL)
        message(FATAL_ERROR "MIRAGE_DISPLAY_PLUGIN_QML requires MIRAGE_DISPLAY_WITH_EGL=ON")
    endif()
    add_subdirectory(plugins/qml)
    install(DIRECTORY extensions/kde/package/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/plasma/wallpapers/org.mirage.wallpaper)

    if(MIRAGE_DISPLAY_QML_URI MATCHES "Embed")
        install(DIRECTORY extensions/kde/package/
            DESTINATION org.mirage.wallpaper
            COMPONENT kde_extension
            EXCLUDE_FROM_ALL
            PATTERN "MirageSurface.qml" EXCLUDE
            PATTERN "MirageSurfaceEmbed.qml" EXCLUDE)
        install(FILES extensions/kde/package/contents/ui/MirageSurfaceEmbed.qml
            DESTINATION org.mirage.wallpaper/contents/ui
            RENAME MirageSurface.qml
            COMPONENT kde_extension
            EXCLUDE_FROM_ALL)
        list(APPEND CPACK_COMPONENTS_ALL kde_extension)
        set(CPACK_ARCHIVE_KDE_EXTENSION_FILE_NAME
            "mirage-kde-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}-embed")
    endif()
endif()

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "mirage-linux-display")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_PACKAGING_INSTALL_PREFIX "")
include(CPack)
