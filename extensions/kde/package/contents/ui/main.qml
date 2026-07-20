import QtQuick
import org.kde.plasma.plasmoid

WallpaperItem {
    id: root

    Rectangle {
        anchors.fill: parent
        color: surfaceLoader.status === Loader.Ready
            ? surfaceLoader.item.clearColor
            : "black"
    }

    WindowModel {
        id: windowModel
        screenGeometry: Qt.rect(Screen.virtualX, Screen.virtualY,
                                Screen.width, Screen.height)
    }

    Loader {
        id: surfaceLoader
        anchors.fill: parent
        asynchronous: false
        source: "MirageSurface.qml"
        onLoaded: {
            item.configuredDisplayName = Qt.binding(function() {
                return root.configuration.DisplayName || "";
            });
            item.configuredPointerForwarding = Qt.binding(function() {
                return root.configuration.MouseForward;
            });
            item.configuredWindowStateFlags = Qt.binding(function() {
                return windowModel.flags;
            });
        }
    }

    Component.onCompleted: root.loading = false
}
