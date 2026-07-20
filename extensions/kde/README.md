# KDE Plasma Adapter

This adapter is a Plasma 6 wallpaper package. Plasma owns the surface on both
X11 and Wayland; the plugin never creates or places an X11 window.

Build and install the QML module and wallpaper package with:

```sh
cmake -S . -B build-kde -G Ninja \
  -DMIRAGE_DISPLAY_PLUGIN_QML=ON \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-kde
cmake --install build-kde
```

The initial renderer path uses Qt Quick's OpenGL/EGL backend. Plasma X11 must
therefore run with its EGL XCB integration rather than GLX. This affects GPU
import only; placement and input remain owned by Plasma. A native Qt Quick
Vulkan path is the next backend milestone.

Window state comes from `org.kde.taskmanager`, which is backed by Plasma/KWin
workspace data and works across both session types. A future KWin script bridge
may expose additional `KWinWorkspaceWrapper` fields without changing the
display protocol.
