# mirage-display-v1 Wire Protocol

Status: frozen for the first implementation.

## Transport

The broker listens on:

```text
$XDG_RUNTIME_DIR/mirage-wallpaper/display-v1.sock
```

The socket is Linux `AF_UNIX`, `SOCK_SEQPACKET`, mode `0600`. Every protocol
message is exactly one sequenced packet. File descriptors are attached to that
packet with `SCM_RIGHTS`.

The broker rejects peers whose `SO_PEERCRED.uid` differs from its own uid.

## Packet Header

All fields are little-endian. The header is exactly 24 bytes.

| Offset | Type | Field | Value |
|---:|---|---|---|
| 0 | `u32` | magic | `0x3150444d`, bytes `MDP1` |
| 4 | `u16` | major | `1` |
| 6 | `u16` | minor | negotiated minor, `0` during hello |
| 8 | `u16` | opcode | message opcode |
| 10 | `u16` | flags | packet flags |
| 12 | `u32` | payload_size | bytes following the header |
| 16 | `u16` | fd_count | descriptors attached by `SCM_RIGHTS` |
| 18 | `u16` | reserved | must be zero |
| 20 | `u32` | serial | monotonically increasing per sender |

Maximum packet size is 65536 bytes, so maximum payload size is 65512 bytes.

Flag bit 0 is `OPTIONAL`. A peer may ignore an unknown opcode only when this bit
is present. Unknown required opcodes are fatal protocol errors.

## Primitive Encoding

- `u16`, `u32`, `u64`: unsigned little-endian integers.
- `f32`: IEEE-754 binary32 encoded as its little-endian `u32` bits.
- `string`: `u32 byte_length` followed by UTF-8 bytes, without NUL.
- `bytes16`: exactly sixteen bytes.
- `array<T>`: `u32 count` followed by `count` encoded values.
- `rect`: four `f32` values: `x`, `y`, `width`, `height`.

Strings are limited to 4096 bytes. Arrays are limited by their message-specific
maximum and by the packet-size limit. Decoders reject trailing bytes.

## Roles

`HELLO.role` is one of:

| Value | Role |
|---:|---|
| 1 | display consumer |
| 2 | render producer |

The broker is the server for both roles.

## Feature Bits

| Bit | Name | Meaning |
|---:|---|---|
| 0 | explicit sync | acquire sync file and release syncobj frame FDs |
| 1 | DRM modifiers | non-linear DMA-BUF modifier negotiation |
| 2 | multiplane | more than one DMA-BUF plane per image |
| 3 | pointer axis | horizontal and vertical scrolling |
| 4 | window state | covering-window facts can be reported |
| 5 | color metadata | reserved for a later minor version |

Version 1 requires explicit sync. Other features are negotiated by intersection.

## Common Handshake

The first message from a peer is `HELLO`. The broker replies with `WELCOME` or a
fatal `ERROR`.

`HELLO` contains:

```text
u32 role
u16 min_minor
u16 max_minor
u64 features
string name
string version
```

`WELCOME` contains:

```text
u16 selected_minor
u16 reserved
u64 features
string server_name
string server_version
```

After `WELCOME`, a display sends `REGISTER_OUTPUT`; a producer sends
`REGISTER_PRODUCER`. No role-specific traffic is valid before registration is
accepted.

## Display Registration

`REGISTER_OUTPUT` contains:

```text
string stable_id
string name
u32 physical_width
u32 physical_height
u32 logical_width
u32 logical_height
u32 scale_120
u32 refresh_mhz
u32 transform
u32 drm_render_major
u32 drm_render_minor
u64 input_caps
```

`scale_120` uses 120 units per logical scale factor. `transform` uses the
`wl_output.transform` numeric values 0 through 7.

The broker replies with `OUTPUT_ACCEPTED { u64 output_id }`. The display then
sends `CONSUMER_CAPS`.

Each format capability is:

```text
u32 fourcc
u32 plane_count
u64 modifier
```

`CONSUMER_CAPS` contains:

```text
u64 sync_caps
u64 color_caps
u32 max_width
u32 max_height
bytes16 device_uuid
bytes16 driver_uuid
array<format_cap> formats
```

At most 256 format capabilities are allowed.

## Buffer Pool

The broker sends `BIND_BUFFERS` only after a producer and consumer have a
compatible negotiated format.

```text
u64 generation
u32 buffer_count
u32 width
u32 height
u32 fourcc
u32 plane_count
u64 modifier
array<plane_desc> descriptors
```

Plane descriptors are ordered buffer-major, then plane-major:

```text
u32 stride
u32 offset
u64 size
```

The descriptor count must equal `buffer_count * plane_count`. The attached FD
count must have the same value. Version 1 permits 2 to 4 buffers and 1 to 4
planes. The consumer library owns these descriptors until unbind or disconnect;
callbacks borrow them.

`SET_CONFIG` contains:

```text
u64 config_generation
rect source
rect destination
u32 transform
f32 clear_r
f32 clear_g
f32 clear_b
f32 clear_a
```

## Frames and Synchronization

`FRAME_READY` contains:

```text
u64 buffer_generation
u32 buffer_index
u32 reserved
u64 sequence
```

It carries exactly two FDs:

1. Acquire `sync_file`, signaled when producer writes are complete.
2. Binary release DRM syncobj FD, initially unsignaled.

Ownership of both descriptors transfers to the frame callback. The consumer
waits on the acquire descriptor before sampling, then signals the release
syncobj after its final GPU read. Closing an unsignaled release descriptor is an
abnormal fallback and may cause the producer to time out the slot.

For Vulkan peers, version 1 fixes the cross-process image state to
`VK_IMAGE_LAYOUT_GENERAL`. Before publishing a frame, the producer releases
queue-family ownership to `VK_QUEUE_FAMILY_FOREIGN_EXT`. A Vulkan consumer
acquires from `VK_QUEUE_FAMILY_FOREIGN_EXT` before its first read and releases
back to that family before signaling the release semaphore. These values are
protocol invariants and are therefore not repeated in each frame packet.

Frames from a generation other than the currently bound generation are closed
and dropped without invoking the frame callback.

## Pool Replacement

The broker sends `UNBIND { u64 generation }`. The consumer performs this order:

1. Stop scheduling new reads from the pool.
2. Wait for or retire all host GPU references.
3. Invoke the releasing callback.
4. Close all library-owned pool FDs.
5. Send `UNBIND_DONE { u64 generation }`.

The C consumer API performs steps 3 through 5 synchronously by default. A
render-thread-backed adapter may call `md_display_defer_unbind()` from its
releasing callback, destroy EGL/Vulkan/Qt Quick references asynchronously, and
then call `md_display_finish_unbind()` on its protocol event thread. The library
keeps the pool and its FDs valid until that explicit completion. The wire
sequence and producer ownership rules do not change.

A new `BIND_BUFFERS` uses a different generation.

## Pointer Input

Coordinates are physical output pixels with a top-left origin. Timestamps use a
monotonic microsecond clock. Modifiers use Linux input modifier bits when known,
or zero.

Pointer motion:

```text
f32 x
f32 y
u64 timestamp_us
u32 modifiers
```

Pointer button:

```text
f32 x
f32 y
u32 button
u32 state
u64 timestamp_us
u32 modifiers
```

Button values are Linux `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE`, `BTN_SIDE` and
`BTN_EXTRA` codes. State is 0 for release and 1 for press.

Pointer axis:

```text
f32 x
f32 y
f32 delta_x
f32 delta_y
u32 source
u64 timestamp_us
u32 modifiers
```

Deltas are logical wheel notches. Sources are wheel=0, finger=1 and
continuous=2. Dragging is reconstructed from motion plus button state.

## Error Handling

`ERROR` contains `u32 code`, `u32 fatal`, and a UTF-8 message. Fatal errors end
the session after delivery. Protocol violations are always fatal.

Peers close all owned descriptors on disconnect. No descriptor received in an
invalid or stale packet may escape its error path.

## Producer Session

A render producer sends `REGISTER_PRODUCER` after the common handshake. It
advertises a stable output identity, renderer kind, DRM render node, device and
driver UUIDs, and its supported `(fourcc, plane_count, modifier)` tuples.

After `PRODUCER_ACCEPTED`, the broker sends `OUTPUT_CONFIG` with the selected
extent and format. The producer allocates a new generation and sends
`OFFER_BUFFERS`, attaching one DMA-BUF FD for each buffer plane. Pool FDs remain
owned by the producer; the protocol library duplicates them for queued sends.

Each `PRODUCER_FRAME` carries the same payload and two synchronization FDs as
the display-side `FRAME_READY`. Frame FD ownership transfers to the producer
protocol library. The broker forwards equivalent descriptors to the display.

The producer sends `PRODUCER_SET_CONFIG` whenever source/destination placement,
transform or renderer-owned clear color changes. The broker forwards the body
unchanged as display-side `SET_CONFIG`.

On `RETIRE_BUFFERS`, the producer stops submitting the generation, waits for
local GPU use to finish, destroys the pool, and sends `RETIRE_DONE`. New frames
for a retiring generation are rejected.
