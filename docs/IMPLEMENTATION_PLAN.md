# Mirage Linux Display Implementation Plan

## 1. Objective

Replace MirageWallpaper's Linux X11-only desktop ownership with a
protocol-driven offscreen renderer. MirageWallpaper exports DMA-BUF frames;
desktop-environment integrations display them in DE-owned wallpaper surfaces and
return pointer input to the renderer.

X11 remains supported. The architectural change is that neither MirageWallpaper
nor this library creates or manages a raw X11 desktop window. Plasma X11,
Plasma Wayland, GNOME X11 and GNOME Wayland are session variants handled by
their respective DE adapters through the interfaces the DE provides.

The existing MirageQt-to-renderer JSON stdin channel remains responsible for
wallpaper controls such as pause, volume, FPS and user properties. It is not
part of the display protocol.

## 2. Target Architecture

```text
SceneWallpaper / WebWallpaper / VideoWallpaper producer
                         |
                 DMA-BUF + explicit sync
                         |
                         v
             MirageQt embedded DisplayBroker
                         |
          $XDG_RUNTIME_DIR/mirage-wallpaper/display-v1.sock
                         |
                         v
                 libmirage-display
             /            |             \
       KDE Plasma     GNOME Shell     layer-shell
       X11/Wayland     X11/Wayland    Wayland only
```

The broker forwards protocol packets and file descriptors. Pixel data remains
in GPU memory and is never copied through the broker.

One broker provides stable discovery, multi-output routing and recovery when a
renderer or desktop shell restarts. A stable output identity replaces the
current screen-index contract.

## 3. Non-Goals for Version 1

- A generic raw-X11 host using Xlib, XCB, XFixes or root-window ownership.
- Bypassing the desktop shell to place or stack wallpaper windows.
- Keyboard capture.
- Touch, tablet or gesture input.
- Network transport or cross-user sessions.
- CPU pixel-stream fallback.
- Wallpaper settings and media-control messages.

## 4. Milestone 0: Protocol Freeze

No renderer, broker or desktop plugin implementation begins until the following
artifacts agree and pass review:

- `protocol/mirage_display_v1.xml`: authoritative message schema.
- `docs/mirage-display-v1.md`: wire format, state machines and ownership rules.
- Checked-in C protocol constants and codecs.
- Binary golden vectors for fixed messages.
- Tests for framing, ancillary FD transfer and malformed packets.

The frozen decisions are:

- Linux `AF_UNIX` with `SOCK_SEQPACKET`, non-blocking after connection.
- `SCM_RIGHTS` for DMA-BUF and synchronization descriptors.
- Little-endian fixed-width integers and IEEE-754 `float32`.
- Length-prefixed UTF-8 strings, without trailing NUL bytes.
- Major-version incompatibility; minor versions and feature bits are additive.
- Strict per-message FD counts and explicit ownership transfer.
- Generation-tagged buffer pools; stale frames are dropped.

## 5. Milestone 1: Protocol Library and Simulator

Implement a stable C ABI suitable for Qt, GObject and Rust FFI:

- Packet codec and `SCM_RIGHTS` handling.
- Non-blocking connect and handshake state machine.
- Output registration and consumer capability advertisement.
- Buffer-pool, configuration, frame and unbind callbacks.
- Pointer enter, leave, motion, button and axis requests.
- Deterministic close and FD cleanup on all error paths.
- Mock broker and headless consumer.
- Unit tests, end-to-end session tests and malformed-packet tests.

GPU import is split into focused submilestones:

1. Raw DMA-BUF and sync FD delivery through the stable C ABI.
2. EGL import helper using `EGL_EXT_image_dma_buf_import`.
3. Vulkan import helper using external memory FD and DRM modifiers.
4. Optional relay/blit path for consumers that cannot sample the producer's
   modifier directly.

Current implementation status:

- Raw DMA-BUF and synchronization FD delivery: implemented.
- Single-plane Vulkan XRGB/ARGB import and per-slot semaphore import: implemented.
- EGLImage import and native-fence synchronization: implemented.
- Renderer producer session, buffer offers, frame submission and input callbacks: implemented.
- Deferred consumer unbind completion for render-thread GPU teardown: implemented.
- Plasma 6 wallpaper/QML adapter using DE-owned surfaces on X11 and Wayland:
  OpenGL/EGL path implemented.
- Multiplane Vulkan import and relay/blit fallback: pending.
- Qt Quick Vulkan backend: pending.

The raw-FD ABI is completed first so EGL and Vulkan helpers cannot alter the
wire contract.

## 6. Milestone 2: MirageWallpaper Broker and Producer

### MirageQt DisplayBroker

- Own the well-known display socket with mode `0600`.
- Validate peers with `SO_PEERCRED`.
- Track consumer, producer and output lifecycles independently.
- Match outputs using a stable DE-provided identity, not list indices.
- Negotiate format/modifier intersections and forward only descriptors.
- Preserve consumers while renderer processes restart.
- Re-send active pool and configuration after consumer reconnect.

### SceneRenderer Producer

- Replace the Linux X11 `DesktopHost` with a display-protocol producer.
- Keep the producer independent of X11, Wayland and desktop-environment APIs.
- Run SceneWallpaper in its existing offscreen mode.
- Add `ProtocolExSwapchain` beside the current local offscreen swapchain.
- Allocate a three-slot Vulkan export pool.
- Export images through `VK_KHR_external_memory_fd` and DRM modifiers.
- Export acquire sync files and provide one release syncobj per submitted frame.
- Never reuse a slot before the release syncobj signals.
- Feed broker input into existing `mouseInput`, `mouseButton` and `mouseEnter`
  APIs. Dragging remains derived from motion plus button state.

## 7. Milestone 3: KDE Plasma

- Build one Plasma 6 wallpaper package and Qt Quick plugin for both Plasma X11
  and Plasma Wayland.
- Let `plasmashell` own the wallpaper surface, placement, stacking, activities
  and virtual desktops; never create a desktop window directly.
- Import DMA-BUF through QRhi/Vulkan where possible, with EGL fallback.
- Report physical size, logical size, scale, transform and refresh rate from
  Plasma/Qt screen objects.
- Forward pointer events observed by the wallpaper item while returning event
  propagation to Plasma.
- Obtain covering-window, active-window and workspace state from Plasma task
  models and KWin workspace APIs such as `KWinWorkspaceWrapper`; do not query
  X11 windows directly.
- Validate mixed-DPI multi-monitor, rotation and hotplug.

KDE is first because MirageQt and Plasma share Qt 6 integration patterns and it
provides the shortest route to validating the complete renderer-to-DE path.

The first implementation lives under `plugins/qml` and
`extensions/kde/package`. It deliberately has no X11 or Wayland window code;
the Plasma `WallpaperItem` is the host on both session types.

## 8. Milestone 4: GNOME Shell

- Build a native GObject library with introspection bindings.
- Build a Shell extension that inserts the live texture into each shell-owned
  background on supported GNOME X11 and Wayland sessions.
- Observe stage pointer events and return `EVENT_PROPAGATE`.
- Obtain window/workspace state from Shell/Mutter APIs, never raw X11 queries.
- Reconnect across Shell extension reloads and session lock transitions.
- Test against the supported GNOME Shell version range.

## 9. Milestone 5: layer-shell Compositors

- Implement a standalone layer-shell consumer.
- Support wlroots compositors first, followed by compositor-specific adapters
  for Hyprland and Niri where needed.
- Advertise actual input capabilities during registration.

Wayland does not provide a universal non-consuming global-input API. KDE and
GNOME plugins can observe events inside their shells. A generic layer-shell
client must not claim pointer features the compositor cannot provide.

## 10. Milestone 6: Remove Direct X11 Ownership

After the DE-managed display path passes end-to-end tests on both session types:

- Remove MirageWallpaper's `X11DesktopHost.cpp` and direct desktop-host
  declarations.
- Remove renderer-side Xlib, XRandR, XFixes and Xext build dependencies.
- Replace `SCENERENDERER_ENABLE_X11_WALLPAPER` with protocol-producer build
  selection.
- Remove MirageQt `DesktopWindowX11` placement helpers and direct session
  gating. Session support is determined by installed DE adapters.
- Replace `--screen` and screen-index routing with stable DE-provided output
  identities.
- Retain Plasma X11 and supported GNOME X11 operation through their DE plugins.
- Do not add a generic raw-X11 fallback.

## 11. Protocol Invariants

- Buffer-pool generation numbers never repeat within one connection.
- A buffer slot has at most one outstanding frame.
- The producer skips a render tick when no released slot exists.
- Pool teardown is `UNBIND -> UNBIND_DONE -> descriptor close`.
- Pool replacement never reuses the old generation.
- Callback payloads are borrowed unless a field explicitly transfers ownership.
- Frame acquire and release descriptors transfer ownership to the consumer.
- Disconnect closes all library-owned descriptors exactly once.
- Pointer coordinates use output physical pixels, with a top-left origin.
- Pointer timestamps use a monotonic microsecond clock.

## 12. Verification Matrix

### Protocol

- Header and primitive golden vectors.
- Short packet, oversized packet and trailing-byte rejection.
- Missing, excess and truncated `SCM_RIGHTS` arrays.
- Unknown optional opcode skip and unknown required opcode disconnect.
- Old-generation frame drop.
- Disconnect during handshake, bind, frame and unbind.
- Repeated connect/close with FD-count monitoring.

### GPU

- Intel Mesa, AMD Mesa and NVIDIA proprietary drivers.
- Same-GPU and PRIME cross-GPU configurations.
- Linear and optimal/modifier-backed images.
- 1x, fractional and mixed output scaling.
- Resize, rotation, hotplug and suspend/resume.

### Desktop Environments

- KDE Plasma Wayland.
- KDE Plasma X11.
- Supported GNOME Shell Wayland versions.
- Supported GNOME Shell X11 versions where the Shell extension API permits the
  same background integration.
- Sway or another wlroots compositor.
- Hyprland and Niri adapters.

## 13. Completion Criteria

- KDE Plasma displays live frames on both X11 and Wayland through the same
  wallpaper plugin.
- GNOME Shell displays live frames on every explicitly supported X11 and
  Wayland release through Shell/Mutter APIs.
- At least two Wayland-only compositors display live frames through layer-shell
  or a compositor-provided wallpaper API.
- Pointer move, click, wheel and button-held drag reach SceneRenderer.
- No steady-state CPU pixel copy occurs.
- Either side may restart and automatically establish a fresh session.
- Resizes and pool generations complete without use-after-free or stale frames.
- Long-running tests show no growth in DMA-BUF, sync FD or Vulkan resources.
