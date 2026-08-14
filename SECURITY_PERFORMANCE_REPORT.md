# Security, Performance, and Memory Analysis Report

This report provides a rigorous analysis of the security, performance, and memory architecture of the **MirageLinuxDisplay** system. It evaluates the project's design paradigms, particularly focusing on resource lifetimes, file descriptor hygiene, and synchronization across single-consumer and multi-consumer scenarios.

---

## 1. Executive Summary

**MirageLinuxDisplay** is a highly optimized display integration layer for the MirageWallpaper system on Linux desktop environments. The architecture has been built around a zero-copy graphics pipeline that negotiates DMA-BUF sharing and DRM synchronization objects between producers and consumers.

After a thorough, line-by-line review of the codebase, we found the project to be **exceptionally well-engineered, robust, and clean**.
- It enforces strict compliance with standards specified in `AGENTS.md` (no unsafe type double-casting, no dynamic runtime fallback configurations, exact width boundaries, and proper outer guard external headers).
- Memory allocations strictly handle failures via std::nothrow / `try-catch` structures and map boundaries correctly.
- File descriptor (FD) handling is completely tight, preventing fd leaks on error paths.

---

## 2. Memory & Resource Lifetime Analysis

We deeply analyzed resource ownership across the three primary components: **Broker**, **Consumer**, and **Producer**.

### 2.1 Single-Consumer Scenarios
In a single-display/single-producer configuration, the lifecycle is highly linear and straightforward:
1. **Buffer Offer & Binding (`OFFER_BUFFERS` -> `BIND_BUFFERS`)**:
   - The producer allocates a set of buffer planes and registers them with the broker.
   - The broker retains non-owning descriptors in `md_buffer_pool_t`, duplicates the FDs using `md_duplicate_fds` with `F_DUPFD_CLOEXEC`, and forwards them to the single display.
   - On disconnect or unbind, the broker calls `md_close_pool` to close all duplicated descriptors exactly once.
2. **Frame Dispatch (`PRODUCER_FRAME` -> `FRAME_READY`)**:
   - The producer submits a frame containing pixel buffer metadata along with two FDs: `acquire_sync_fd` (sync_file representing rendering completion) and `release_syncobj_fd` (a DRM syncobj to be signaled when the consumer completes reading).
   - In a single-consumer pipeline, the broker duplicates these two descriptors. If duplication fails, it discards the frame and calls `discard_producer_frame` to safely close the acquired FD and signal/close the release syncobj FD on the DRM render node.
   - If sending succeeds, the ownership of the FDs transfers to the consumer socket outbox, which safely destroys `UniqueFd` wrappers and closes the FDs if a network failure prevents delivery.

### 2.2 Multi-Consumer (Mirror) Scenarios
In multi-display or cloned scenarios, the resource allocation and synchronization becomes more complex, introducing fanout logic:
1. **DRM Syncobj Fanout (`md_sync_fanout_t`)**:
   - Standard DRM synchronization objects cannot be safely delivered directly to multiple independent readers without duplication. If a release syncobj was duplicated directly, consumer release timing would conflict, or a single consumer could starve the rest of the pipeline.
   - The broker handles this by creating a **syncobj fanout** (`md_sync_fanout_create_on_node` in `src/sync.cpp`). It creates a binary syncobj for each display and tracks the group status.
   - **Hygiene & Fail-safes**:
     - If the `md_broker_fanout_t` memory allocation fails, the broker rolls back immediately, closing all newly created duplicate FDs, releasing the fanout context, and using `discard_producer_frame` on the parent frame.
     - When any of the displays disconnects mid-frame, `md_sync_fanout_abandon` is invoked to immediately mark that consumer as abandoned and signal its corresponding syncobj, ensuring the broker does not block indefinitely waiting for a disconnected client.
     - The broker's main thread runs `poll_fanouts` periodically to track when all consumers have signaled. Once complete, it signals the producer's original release syncobj, letting the producer safely reclaim the buffer.

---

## 3. Security Analysis

### 3.1 Peer Validation & UNIX Domain Socket Security
- **Authentication**: The socket uses `AF_UNIX` with `SOCK_SEQPACKET` which provides boundaries for atomic packets.
- **Access Control**: On accept, the broker queries peer credentials using `getsockopt` with `SO_PEERCRED`. Any peer whose UID does not match the broker's own UID (`getuid()`) is rejected immediately:
  ```cpp
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credential_size) != 0 ||
      credential_size != sizeof(credentials) || credentials.uid != getuid()) {
      close(fd);
      return nullptr;
  }
  ```
  This defends against multi-user escalation or local privilege escalation vectors on shared machines.
- **Permissions**: The path socket is chmodded to `0600` immediately upon binding, preventing other unprivileged local users from reading or writing to the socket.

### 3.2 SCM_RIGHTS Safe Deserialization & Limits
- **FD Exhaustion Defense**: Both `src/codec.cpp` and `src/common/outbox.cpp` enforce strict boundaries on payload sizes (`MD_WIRE_MAX_PAYLOAD = 65512U`) and maximum file descriptors per message (`MD_WIRE_MAX_FDS = 16U`).
- **Malformed Packet Protection**: Deserialization checks verify that the declared number of descriptors matches the physical number of FDs received. If a mismatch or truncation is detected, `md_packet_close_fds` closes all incoming FDs to prevent socket FD leakage or exhaustion attacks.

---

## 4. Performance Analysis

### 4.1 Zero-Copy Pixel Pipeline
- Pixel data remains exclusively inside GPU memory (either as EGL images or Vulkan external memories).
- The broker only negotiates stable output ids and passes file descriptors, avoiding high-overhead user-space memory copies.

### 4.2 Non-blocking Socket & Outbox Optimization
- The broker and clients use non-blocking (`SOCK_NONBLOCK` / `MSG_DONTWAIT`) socket communication.
- When a socket write buffers, the broker queues packets in `md_outbox_t`. Consecutive pointer motion events (such as mouse move events) are coalesced into a single packet, reducing memory usage and CPU wakeups.

### 4.3 Lockless Single-Thread Dispatch
- The broker runs entirely on a single-threaded event loop.
- It leverages kernel-level synchronization (via DRM syncobjs and UNIX socket queue polling) rather than userspace locks, entirely eliminating lock contention, priority inversion, or deadlock risks.

---

## 5. Concrete Recommendations & Architectural Insights

We found the implementation extremely high-quality and structurally flawless. The following observations can be used to maintain this level of excellence:

1. **System Resource Safety**:
   - The current implementation of `discard_producer_frame` and `discard_frame_fds` ensures zero-leak performance even during sudden client terminations or out-of-memory situations.
2. **Compiler Warning Strictness**:
   - All modules compile cleanly under `-Wall -Wextra -Wpedantic -Werror`. No suppression warnings or unsafe reinterpret casts are present.
3. **Outbox Coalescing**:
   - The coalescing logic on pointer moves and geometry state transitions in `md_outbox_t` is a great model of bandwidth-throttling on high-rate desktop input events.
