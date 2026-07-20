# MirageLinuxDisplay

Linux desktop-environment display integration for MirageWallpaper.

The project defines the `mirage-display-v1` Unix-domain-socket protocol and a
stable C consumer library. KDE Plasma, GNOME Shell and compositor-specific
integrations use the library to receive DMA-BUF frames from MirageWallpaper and
forward desktop input back to the renderer.

Both X11 and Wayland sessions are supported through interfaces owned by the
active desktop environment. This project intentionally does not create a raw
X11 desktop/root window. For example, the KDE integration is a Plasma wallpaper
plugin on both Plasma X11 and Plasma Wayland, with workspace information coming
from Plasma/KWin APIs.

The implementation is protocol-first. See:

- `docs/IMPLEMENTATION_PLAN.md`
- `docs/mirage-display-v1.md`
- `docs/desktop-adapters.md`
- `protocol/mirage_display_v1.xml`

The reusable broker core is exposed as `mirage_display_broker.h`. It owns a
`0600` Unix socket, validates same-UID peers, matches one producer and one DE
consumer per stable output identity, negotiates exact format/modifier tuples,
and forwards DMA-BUF/synchronization descriptors without copying pixels.

## Build

```sh
cmake -S . -B build -G Ninja -DMIRAGE_DISPLAY_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

To build the Plasma 6 adapter and wallpaper package, enable the optional QML
target (it currently uses Qt Quick OpenGL/EGL for DMA-BUF import):

```sh
cmake -S . -B build-kde -G Ninja \
  -DMIRAGE_DISPLAY_PLUGIN_QML=ON \
  -DMIRAGE_DISPLAY_WITH_EGL=ON
cmake --build build-kde
```

The Vulkan DMA-BUF helper is built automatically when Vulkan headers and the
loader are available. Disable it with `-DMIRAGE_DISPLAY_WITH_VULKAN=OFF`.
The EGLImage helper is likewise controlled by `MIRAGE_DISPLAY_WITH_EGL`.
