# KDE Plasma Adapter

This adapter is a Plasma 6 wallpaper package. Plasma owns the surface on both
X11 and Wayland; the plugin never creates or places an X11 window.

The build requires CMake 3.20, Ninja, Qt 6.5 or newer (Gui, Qml and Quick),
pkg-config, and EGL/GLESv2 development packages. `kpackagetool6` is required
for installation. Vulkan support is detected automatically.

Build the standalone package from the repository root. The build directory is
only a staging area; the build does not install libraries or QML modules into
system paths. The embedded `Mirage.DisplayEmbed` module is included in the
wallpaper ZIP:

```sh
cmake -S extensions/kde -B build-kde-package -G Ninja
cmake --build build-kde-package --target package
# build-kde-package/mirage-wallpaper-0.1.0.zip
```

Install it for the current user with KDE's package manager:

```sh
kpackagetool6 --type=Plasma/Wallpaper \
  --install build-kde-package/mirage-wallpaper-0.1.0.zip
```

The initial renderer path uses Qt Quick's OpenGL/EGL backend. Plasma X11 must
therefore run with its EGL XCB integration rather than GLX. This affects GPU
import only; placement and input remain owned by Plasma. A native Qt Quick
Vulkan path is the next backend milestone.

Window state comes from `org.kde.taskmanager`, which is backed by Plasma/KWin
workspace data and works across both session types. A future KWin script bridge
may expose additional `KWinWorkspaceWrapper` fields without changing the
display protocol.
