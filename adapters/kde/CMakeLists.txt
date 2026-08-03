add_subdirectory(qml)

install(DIRECTORY wallpaper/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/plasma/wallpapers/org.mirage.wallpaper)

if(MIRAGE_DISPLAY_QML_URI MATCHES "Embed")
    install(DIRECTORY wallpaper/
        DESTINATION org.mirage.wallpaper
        COMPONENT kde_extension
        EXCLUDE_FROM_ALL
        PATTERN "MirageSurface.qml" EXCLUDE
        PATTERN "MirageSurfaceEmbed.qml" EXCLUDE)
    install(FILES wallpaper/contents/ui/MirageSurfaceEmbed.qml
        DESTINATION org.mirage.wallpaper/contents/ui
        RENAME MirageSurface.qml
        COMPONENT kde_extension
        EXCLUDE_FROM_ALL)
    list(APPEND CPACK_COMPONENTS_ALL kde_extension)
    set(CPACK_ARCHIVE_KDE_EXTENSION_FILE_NAME
        "mirage-kde-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}-embed")
endif()

# ---------------------------------------------------------------------------
# Installable Plasma/Wallpaper kpackage archive
#
# Produces a ZIP that can be installed directly with
#     kpackagetool6 -t Plasma/Wallpaper -i mirage-wallpaper-<version>.zip
# The archive has the KPackage layout: metadata.json at the archive root plus
# a contents/ tree (ui/main.qml is the Plasma/Wallpaper mainscript). This is
# a different artifact from the CPack library distribution
# (mirage-linux-display-<version>-Linux.zip), which kpackagetool6 cannot
# consume.
# ---------------------------------------------------------------------------
set(_wallpaper_pkg_name "org.mirage.wallpaper")
set(_wallpaper_pkg_stage
    "${CMAKE_CURRENT_BINARY_DIR}/wallpaper-pkg/${_wallpaper_pkg_name}")
set(_wallpaper_pkg_archive
    "${CMAKE_CURRENT_BINARY_DIR}/mirage-wallpaper-${PROJECT_VERSION}.zip")
set(_wallpaper_pkg_embed OFF)
if(MIRAGE_DISPLAY_QML_URI MATCHES "Embed")
    set(_wallpaper_pkg_embed ON)
    set(_wallpaper_pkg_archive
        "${CMAKE_CURRENT_BINARY_DIR}/mirage-wallpaper-${PROJECT_VERSION}-embed.zip")
endif()

set(_wallpaper_pkg_script
    "${CMAKE_CURRENT_BINARY_DIR}/package-wallpaper.cmake")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/package-wallpaper.cmake.in"
    "${_wallpaper_pkg_script}"
    @ONLY)

file(GLOB _wallpaper_pkg_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/wallpaper/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/wallpaper/contents/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/wallpaper/contents/config/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/wallpaper/contents/ui/*")

add_custom_command(
    OUTPUT "${_wallpaper_pkg_archive}"
    COMMAND "${CMAKE_COMMAND}" -P "${_wallpaper_pkg_script}"
    DEPENDS
        "${_wallpaper_pkg_script}"
        ${_wallpaper_pkg_files}
    COMMENT "Creating Plasma wallpaper package ${_wallpaper_pkg_archive}")

add_custom_target(mirage-wallpaper-package ALL
    DEPENDS "${_wallpaper_pkg_archive}")
