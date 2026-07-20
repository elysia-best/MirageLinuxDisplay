import QtQuick
import Mirage.Display 1.0

MirageDisplayItem {
    id: display

    property string configuredDisplayName: ""
    property bool configuredPointerForwarding: true
    property int configuredWindowStateFlags: 0

    readonly property string screenIdentity: [
        Screen.manufacturer || "",
        Screen.model || "",
        Screen.serialNumber || "",
        Screen.name || ""
    ].join("|")

    outputStableId: "kde:" + screenIdentity
    outputName: configuredDisplayName.length > 0
        ? configuredDisplayName
        : (Screen.model || Screen.name || "KDE wallpaper")
    physicalWidth: Math.max(1, Math.round(width * Screen.devicePixelRatio))
    physicalHeight: Math.max(1, Math.round(height * Screen.devicePixelRatio))
    logicalWidth: Math.max(1, Math.round(width))
    logicalHeight: Math.max(1, Math.round(height))
    scale120: Math.max(1, Math.round(Screen.devicePixelRatio * 120))
    // MirageDisplayItem also refreshes this from QScreen when the item is
    // attached to a window. Keeping a numeric fallback avoids relying on
    // compositor-specific QML Screen extensions.
    refreshMhz: 60000
    outputTransform: {
        switch (Screen.orientation) {
        case Qt.PortraitOrientation: return MirageDisplayItem.Transform90;
        case Qt.InvertedLandscapeOrientation: return MirageDisplayItem.Transform180;
        case Qt.InvertedPortraitOrientation: return MirageDisplayItem.Transform270;
        default: return MirageDisplayItem.TransformNormal;
        }
    }
    pointerForwarding: configuredPointerForwarding
    windowStateFlags: configuredWindowStateFlags
}
