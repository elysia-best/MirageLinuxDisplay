#include "mirage_display.h"
#include "mirage_display_broker.h"

/* Keep assertions live even in Release builds (-DNDEBUG), so test
 * binaries still exercise the checks they were written for. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Regression tests for the pathname-socket endpoint lifecycle that the KDE
 * wallpaper depends on.
 *
 * md_broker_listen() unlinks an existing pathname socket and binds a fresh one
 * (see src/broker.cpp), so a consumer that connected to an earlier endpoint
 * keeps a descriptor to an inode nobody listens on any more. The existing
 * broker test only exercises abstract sockets, which have no filesystem inode
 * and therefore cannot express this failure at all. These tests pin down:
 *
 *   A. a consumer created before the broker exists can connect once the broker
 *      binds the endpoint (the normal self-heal path);
 *   B. after the broker replaces the endpoint, a consumer session bound to the
 *      old endpoint no longer reaches the broker, while a fresh session against
 *      the new endpoint completes the handshake;
 *   C. the endpoint identity (st_dev + st_ino) really does change across the
 *      replacement, which is the signal the KDE adapter uses to notice it.
 */

typedef struct broker_thread {
    md_broker_t* broker{};
    pthread_t thread{};
    bool running{};
} broker_thread_t;

typedef struct consumer_observer {
    unsigned connected{};
    unsigned disconnected{};
    uint64_t output_id{};
} consumer_observer_t;

static void* broker_main(void* opaque) {
    broker_thread_t* const thread = static_cast<broker_thread_t*>(opaque);
    for (;;) {
        int32_t rc = md_broker_dispatch(thread->broker, 20);
        if (rc == MD_ERR_DISCONNECTED) break;
        assert(rc >= 0);
    }
    return nullptr;
}

static void on_connected(void* opaque, uint64_t output_id) {
    consumer_observer_t* const observer = static_cast<consumer_observer_t*>(opaque);
    assert(output_id != 0U);
    observer->output_id = output_id;
    ++observer->connected;
}

static void on_disconnected(void* opaque, md_result_t, const char*) {
    consumer_observer_t* const observer = static_cast<consumer_observer_t*>(opaque);
    ++observer->disconnected;
}

/* Fills the consumer registration payload shared by every session below. */
static void fill_output(md_output_info_t* output) {
    memset(output, 0, sizeof(*output));
    output->stable_id = "kde:endpoint-test";
    output->name = "Endpoint test output";
    output->physical_width = 1920U;
    output->physical_height = 1080U;
    output->logical_width = 1920U;
    output->logical_height = 1080U;
    output->scale_120 = 120U;
    output->refresh_mhz = 60000U;
    output->transform = MD_TRANSFORM_NORMAL;
    output->drm_render_major = 226U;
    output->drm_render_minor = 128U;
    output->input_caps = MD_INPUT_POINTER_MOTION | MD_INPUT_POINTER_BUTTON;
}

static void fill_caps(md_consumer_caps_t* caps, const md_format_cap_t* format) {
    memset(caps, 0, sizeof(*caps));
    caps->features = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_POINTER_AXIS |
                     MD_FEATURE_WINDOW_STATE;
    caps->sync_caps = 1U;
    caps->max_width = 16384U;
    caps->max_height = 16384U;
    caps->formats = format;
    caps->format_count = 1U;
}

/*
 * Drives the nonblocking handshake with poll(2) until it is READY or fails.
 * Returns true when the session reached READY within the deadline; the caller
 * asserts on the expected outcome so both success and failure are testable.
 */
static bool drive_handshake(md_display_t* display, int timeout_ms) {
    const int step_ms = 20;
    for (int waited = 0; waited <= timeout_ms; waited += step_ms) {
        int32_t rc = md_display_advance_handshake(display);
        if (rc == MD_HANDSHAKE_DONE) return true;
        if (rc < 0) return false;
        if (rc == MD_HANDSHAKE_PROGRESS) continue;
        const int32_t fd = md_display_get_fd(display);
        if (fd < 0) return false;
        struct pollfd descriptor{
            .fd = fd,
            .events = static_cast<short>(rc == MD_HANDSHAKE_NEED_WRITE ? POLLOUT : POLLIN),
            .revents = 0,
        };
        /* EINTR is the only expected failure here; anything else is a bug in
         * the test harness rather than in the library under test. */
        int poll_rc = poll(&descriptor, 1, step_ms);
        assert(poll_rc >= 0 || errno == EINTR);
    }
    return false;
}

/* Starts a broker on the given pathname socket and returns its dispatch thread. */
static md_broker_t* start_broker(const char* socket_path, broker_thread_t* thread,
                                 bool* unsupported) {
    md_broker_options_t options = {
        .socket_path = socket_path,
        .server_name = "endpoint-test-broker",
        .server_version = "0.2",
        .features = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_POINTER_AXIS |
                    MD_FEATURE_WINDOW_STATE,
        .max_routes = 4,
        .on_window_state = nullptr,
        .user_data = nullptr,
    };
    md_broker_t* broker = md_broker_new(&options);
    assert(broker != nullptr);
    md_result_t listen_result = md_broker_listen(broker);
    if (listen_result != MD_OK) {
        /* Sandboxes without a writable runtime directory cannot bind pathname
         * sockets; the caller turns this into a ctest SKIP instead of a fail. */
        if (errno == EPERM || errno == EACCES || errno == EROFS) {
            md_broker_free(broker);
            *unsupported = true;
            return nullptr;
        }
        assert(listen_result == MD_OK);
    }
    thread->broker = broker;
    assert(pthread_create(&thread->thread, nullptr, broker_main, thread) == 0);
    thread->running = true;
    return broker;
}

static void stop_broker(broker_thread_t* thread) {
    if (!thread->running) return;
    md_broker_stop(thread->broker);
    assert(pthread_join(thread->thread, nullptr) == 0);
    md_broker_free(thread->broker);
    thread->broker = nullptr;
    thread->running = false;
}

/* Returns the endpoint identity the KDE adapter compares across reconnects. */
static bool endpoint_identity(const char* path, dev_t* device, ino_t* inode) {
    struct stat info{};
    if (stat(path, &info) != 0) return false;
    *device = info.st_dev;
    *inode = info.st_ino;
    return true;
}

int main(void) {
    char directory[128];
    assert(snprintf(directory, sizeof(directory), "/tmp/mirage-endpoint-test-%ld",
                    static_cast<long>(getpid())) > 0);
    if (mkdir(directory, 0700) != 0) {
        if (errno == EPERM || errno == EACCES || errno == EROFS) return 77;
        assert(errno == EEXIST);
    }
    char socket_path[192];
    assert(snprintf(socket_path, sizeof(socket_path), "%s/display-v1.sock", directory) > 0);

    md_format_cap_t format = {
        .fourcc = UINT32_C(0x34325258),
        .plane_count = 1,
        .modifier = 0,
    };
    md_output_info_t output;
    md_consumer_caps_t caps;
    fill_output(&output);
    fill_caps(&caps, &format);

    consumer_observer_t observer{};
    md_display_callbacks_t callbacks{};
    callbacks.on_connected = on_connected;
    callbacks.on_disconnected = on_disconnected;
    callbacks.user_data = &observer;

    /* Case A: the broker does not exist yet, so the first attempt must fail
     * without leaving the consumer able to reuse that dead session. */
    md_display_t* early = md_display_new(&callbacks);
    assert(early != nullptr);
    assert(md_display_begin_connect(early, socket_path, "endpoint-test", "0.2",
                                    &output, &caps) == MD_ERR_IO);
    assert(md_display_connection_state(early) == MD_CONNECTION_DEAD);
    assert(observer.disconnected == 1U);
    md_display_free(early);

    broker_thread_t first_thread{};
    bool unsupported = false;
    md_broker_t* first_broker = start_broker(socket_path, &first_thread, &unsupported);
    if (unsupported) {
        const int remove_result = rmdir(directory);
        assert(remove_result == 0 || errno == ENOENT);
        return 77;
    }
    assert(first_broker != nullptr);

    dev_t first_device = 0;
    ino_t first_inode = 0;
    assert(endpoint_identity(socket_path, &first_device, &first_inode));

    /* Case A continued: a fresh session against the now-existing endpoint must
     * complete the handshake, which is the self-heal the wallpaper relies on. */
    md_display_t* healed = md_display_new(&callbacks);
    assert(healed != nullptr);
    assert(md_display_begin_connect(healed, socket_path, "endpoint-test", "0.2",
                                    &output, &caps) == MD_OK);
    assert(drive_handshake(healed, 3000));
    assert(observer.connected == 1U);
    assert(md_display_connection_state(healed) == MD_CONNECTION_READY);

    /* Case B: a second broker replaces the endpoint. md_broker_listen() unlinks
     * the old socket file and binds a new inode, so the still-open session above
     * is attached to an endpoint that no longer has a listener. */
    stop_broker(&first_thread);
    broker_thread_t second_thread{};
    md_broker_t* second_broker = start_broker(socket_path, &second_thread, &unsupported);
    if (unsupported) {
        md_display_free(healed);
        const int unlink_result = unlink(socket_path);
        assert(unlink_result == 0 || errno == ENOENT);
        const int remove_result = rmdir(directory);
        assert(remove_result == 0 || errno == ENOENT);
        return 77;
    }
    assert(second_broker != nullptr);

    /* Case C: the endpoint identity changed, which is exactly the signal the KDE
     * adapter uses to notice a replacement instead of waiting for an I/O error. */
    dev_t second_device = 0;
    ino_t second_inode = 0;
    assert(endpoint_identity(socket_path, &second_device, &second_inode));
    assert(second_device != first_device || second_inode != first_inode);

    /* The old session cannot reach the new broker: draining it either reports a
     * dead peer or simply never reaches READY again. Either way the consumer has
     * to build a new session, which is what the adapter now does. */
    const unsigned disconnects_before = observer.disconnected;
    for (int iteration = 0; iteration < 25; ++iteration) {
        if (md_display_dispatch(healed) < 0) break;
        struct pollfd descriptor{
            .fd = md_display_get_fd(healed),
            .events = POLLIN,
            .revents = 0,
        };
        int poll_rc = poll(&descriptor, 1, 20);
        assert(poll_rc >= 0 || errno == EINTR);
    }
    md_display_free(healed);

    /* Case B continued: a brand-new session against the replaced endpoint must
     * succeed, proving the broker is healthy and only the stale session was bad. */
    md_display_t* rebound = md_display_new(&callbacks);
    assert(rebound != nullptr);
    assert(md_display_begin_connect(rebound, socket_path, "endpoint-test", "0.2",
                                    &output, &caps) == MD_OK);
    assert(drive_handshake(rebound, 3000));
    assert(md_display_connection_state(rebound) == MD_CONNECTION_READY);
    assert(observer.connected == 2U);
    assert(observer.disconnected > disconnects_before);

    md_display_close(rebound);
    md_display_free(rebound);
    stop_broker(&second_thread);
    const int unlink_result = unlink(socket_path);
    assert(unlink_result == 0 || errno == ENOENT);
    const int remove_result = rmdir(directory);
    assert(remove_result == 0 || errno == ENOENT);
    return 0;
}
