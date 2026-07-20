# Desktop Adapter Architecture

## Boundary

`mirage-display-v1` transports frames, output metadata, synchronization objects
and input events. It does not place windows and does not expose X11 or Wayland
objects. The display consumer must run inside an integration point owned by the
desktop environment.

This separation is mandatory:

```text
MirageWallpaper renderer
  -> mirage-display-v1 producer
  -> MirageQt broker
  -> libmirage-display consumer
  -> DE wallpaper/background API
  -> compositor or X server, as selected by the DE
```

The final arrow belongs to the desktop environment. A consumer must not replace
it with an Xlib/XCB desktop window or a private Wayland toplevel.

## KDE Plasma

The KDE adapter is a Plasma 6 `Plasma/Wallpaper` package backed by a Qt Quick
module. The same package runs in `plasmashell` on Plasma X11 and Plasma Wayland.
It must not branch into a raw X11 host.

Responsibilities are divided as follows:

| Layer | Responsibility |
|---|---|
| Plasma `WallpaperItem` | Own the per-screen wallpaper surface and its lifecycle |
| Qt Quick display item | Import DMA-BUF frames, paint them, and observe pointer events |
| Plasma/KWin workspace bridge | Report covering, active, maximized and fullscreen windows |
| `libmirage-display` | Protocol handshake, pools, frames, sync FDs and input messages |

Output identity is derived from the `QScreen` name, manufacturer, model and
serial exposed by Plasma/Qt. Geometry and device pixel ratio come from the
wallpaper item's `Screen` object. Window and workspace state comes from Plasma
task models or KWin workspace interfaces such as `KWinWorkspaceWrapper`.

Pointer observation must return `false` from the Qt event filter so Plasma
continues to receive desktop clicks, context menus, drag/drop and wheel events.
The renderer reconstructs dragging from motion events plus button state.

## GNOME Shell

The GNOME adapter is a Shell extension plus a native texture-import helper. It
inserts a texture into Shell-owned background actors, observes stage events
while returning `Clutter.EVENT_PROPAGATE`, and reads window/workspace state from
Shell/Mutter APIs. If a GNOME X11 release exposes the required Shell APIs, the
same extension path is used; no root-window renderer is introduced.

## Generic Compositors

A generic adapter is permitted only when the compositor has no desktop-shell
wallpaper API. Wayland layer-shell is the first fallback. Compositor-specific
APIs take precedence when they provide better output identity, input observation
or wallpaper lifecycle semantics.

There is intentionally no generic X11 fallback. X11 support is supplied by the
desktop environment adapter, because only the DE can reliably preserve desktop
stacking, activities, virtual desktops, shell input and restart behavior.
