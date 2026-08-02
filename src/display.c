#define _GNU_SOURCE

#include "mirage_display.h"

#include "codec.h"
#include "protocol.h"
#include "sync_fanout.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MD_OUTBOX_LIMIT 64u

static int fill_unix_address(const char* path, struct sockaddr_un* address,
                             socklen_t* address_length) {
    if (path == NULL || address == NULL || address_length == NULL) return MD_ERR_INVALID;
    size_t length = strlen(path);
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    if (path[0] == '@') {
        if (length <= 1u || length >= sizeof(address->sun_path)) return MD_ERR_INVALID;
        memcpy(address->sun_path + 1, path + 1, length - 1u);
        *address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length);
    } else {
        if (length == 0 || length >= sizeof(address->sun_path)) return MD_ERR_INVALID;
        memcpy(address->sun_path, path, length + 1u);
        *address_length = (socklen_t)sizeof(*address);
    }
    return MD_OK;
}

typedef struct md_out_message {
    struct md_out_message* next;
    uint16_t opcode;
    uint16_t flags;
    uint32_t serial;
    size_t size;
    uint8_t payload[];
} md_out_message_t;

struct md_display {
    md_display_callbacks_t callbacks;
    int fd;
    md_connection_state_t connection_state;
    md_handshake_state_t handshake_state;
    uint16_t selected_minor;
    uint64_t negotiated_features;
    uint64_t output_id;
    uint32_t next_serial;
    bool disconnected_notified;

    char* socket_path;
    char* client_name;
    char* client_version;
    char* stable_id;
    char* output_name;
    md_output_info_t output;
    md_consumer_caps_t caps;
    md_format_cap_t* formats;

    uint16_t handshake_opcode;
    uint32_t handshake_serial;
    size_t handshake_size;
    uint8_t handshake_payload[MD_WIRE_MAX_PAYLOAD];

    md_out_message_t* out_head;
    md_out_message_t* out_tail;
    size_t out_count;

    bool pool_active;
    bool in_unbind_callback;
    bool unbind_deferred;
    uint64_t pending_unbind_generation;
    md_buffer_pool_t pool;
};

static char* duplicate_string(const char* value) {
    if (value == NULL) return NULL;
    size_t size = strlen(value) + 1u;
    char* copy = malloc(size);
    if (copy != NULL) memcpy(copy, value, size);
    return copy;
}

static void free_connection_data(md_display_t* display) {
    free(display->socket_path);
    free(display->client_name);
    free(display->client_version);
    free(display->stable_id);
    free(display->output_name);
    free(display->formats);
    display->socket_path = NULL;
    display->client_name = NULL;
    display->client_version = NULL;
    display->stable_id = NULL;
    display->output_name = NULL;
    display->formats = NULL;
    memset(&display->output, 0, sizeof(display->output));
    memset(&display->caps, 0, sizeof(display->caps));
}

static void clear_outbox(md_display_t* display) {
    md_out_message_t* message = display->out_head;
    while (message != NULL) {
        md_out_message_t* next = message->next;
        free(message);
        message = next;
    }
    display->out_head = NULL;
    display->out_tail = NULL;
    display->out_count = 0;
}

static void init_pool(md_buffer_pool_t* pool) {
    memset(pool, 0, sizeof(*pool));
    for (size_t b = 0; b < MIRAGE_DISPLAY_MAX_BUFFERS; ++b) {
        for (size_t p = 0; p < MIRAGE_DISPLAY_MAX_PLANES; ++p) pool->planes[b][p].fd = -1;
    }
}

static void release_pool(md_display_t* display, bool notify) {
    if (!display->pool_active) return;
    if (notify && display->callbacks.on_buffers_releasing != NULL) {
        display->callbacks.on_buffers_releasing(display->callbacks.user_data, &display->pool);
    }
    for (uint32_t b = 0; b < display->pool.buffer_count; ++b) {
        for (uint32_t p = 0; p < display->pool.plane_count; ++p) {
            if (display->pool.planes[b][p].fd >= 0) close(display->pool.planes[b][p].fd);
            display->pool.planes[b][p].fd = -1;
        }
    }
    init_pool(&display->pool);
    display->pool_active = false;
}

static void abandon_pool(md_display_t* display, bool notify_if_needed) {
    bool notify = notify_if_needed && display->pending_unbind_generation == 0;
    release_pool(display, notify);
    display->in_unbind_callback = false;
    display->unbind_deferred = false;
    display->pending_unbind_generation = 0;
}

static md_result_t map_io_error(int error) {
    if (error == -ENOMEM) return MD_ERR_NOMEM;
    if (error == -EPROTO || error == -EMSGSIZE) return MD_ERR_PROTOCOL;
    if (error == -ECONNRESET || error == -EPIPE || error == -ENOTCONN) {
        return MD_ERR_DISCONNECTED;
    }
    return MD_ERR_IO;
}

static int fail_session(md_display_t* display, md_result_t reason, const char* message) {
    if (display->fd >= 0) close(display->fd);
    display->fd = -1;
    abandon_pool(display, true);
    clear_outbox(display);
    display->connection_state = MD_CONNECTION_DEAD;
    display->handshake_state = MD_HANDSHAKE_IDLE;
    if (!display->disconnected_notified && display->callbacks.on_disconnected != NULL) {
        display->disconnected_notified = true;
        display->callbacks.on_disconnected(display->callbacks.user_data, reason,
                                           message != NULL ? message : "session failed");
    }
    return reason;
}

static bool valid_output(const md_output_info_t* output) {
    return output != NULL && output->stable_id != NULL && output->name != NULL &&
           output->physical_width > 0 && output->physical_height > 0 &&
           output->logical_width > 0 && output->logical_height > 0 &&
           output->scale_120 > 0 && output->transform <= MD_TRANSFORM_FLIPPED_270;
}

static bool valid_caps(const md_consumer_caps_t* caps) {
    if (caps == NULL || caps->format_count > MIRAGE_DISPLAY_MAX_FORMATS ||
        (caps->format_count > 0 && caps->formats == NULL)) return false;
    for (uint32_t i = 0; i < caps->format_count; ++i) {
        if (caps->formats[i].plane_count < 1 ||
            caps->formats[i].plane_count > MIRAGE_DISPLAY_MAX_PLANES) return false;
    }
    return true;
}

static int copy_connect_args(md_display_t* display, const char* socket_path,
                             const char* client_name, const char* client_version,
                             const md_output_info_t* output, const md_consumer_caps_t* caps) {
    if (socket_path == NULL || client_name == NULL || client_version == NULL ||
        !valid_output(output) || !valid_caps(caps)) return MD_ERR_INVALID;

    free_connection_data(display);
    display->socket_path = duplicate_string(socket_path);
    display->client_name = duplicate_string(client_name);
    display->client_version = duplicate_string(client_version);
    display->stable_id = duplicate_string(output->stable_id);
    display->output_name = duplicate_string(output->name);
    if (display->socket_path == NULL || display->client_name == NULL ||
        display->client_version == NULL || display->stable_id == NULL ||
        display->output_name == NULL) {
        free_connection_data(display);
        return MD_ERR_NOMEM;
    }

    display->output = *output;
    display->output.stable_id = display->stable_id;
    display->output.name = display->output_name;
    display->caps = *caps;
    display->caps.features |= MD_FEATURE_EXPLICIT_SYNC;
    if (caps->format_count > 0) {
        display->formats = malloc(sizeof(*display->formats) * caps->format_count);
        if (display->formats == NULL) {
            free_connection_data(display);
            return MD_ERR_NOMEM;
        }
        memcpy(display->formats, caps->formats, sizeof(*display->formats) * caps->format_count);
        display->caps.formats = display->formats;
    }
    return MD_OK;
}

static int prepare_handshake(md_display_t* display, uint16_t opcode) {
    md_writer_t writer;
    md_writer_init(&writer, display->handshake_payload, sizeof(display->handshake_payload));
    int rc;
    switch (opcode) {
    case MD_OP_HELLO:
        rc = md_proto_encode_hello(&writer, 1, display->client_name, display->client_version,
                                   display->caps.features);
        break;
    case MD_OP_REGISTER_OUTPUT:
        rc = md_proto_encode_register_output(&writer, &display->output);
        break;
    case MD_OP_CONSUMER_CAPS:
        rc = md_proto_encode_consumer_caps(&writer, &display->caps);
        break;
    default:
        return MD_ERR_INVALID;
    }
    if (rc != 0) return rc == -ENOMEM ? MD_ERR_NOMEM : MD_ERR_INVALID;
    display->handshake_opcode = opcode;
    display->handshake_serial = display->next_serial++;
    display->handshake_size = writer.size;
    return MD_OK;
}

static int start_connected_fd(md_display_t* display, int fd) {
    int status_flags = fcntl(fd, F_GETFL);
    int descriptor_flags = fcntl(fd, F_GETFD);
    if (status_flags < 0 || descriptor_flags < 0 ||
        fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0 ||
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
        return MD_ERR_IO;
    }
    display->fd = fd;
    display->connection_state = MD_CONNECTION_HANDSHAKING;
    display->handshake_state = MD_HANDSHAKE_HELLO_SEND;
    display->disconnected_notified = false;
    display->selected_minor = 0;
    display->negotiated_features = 0;
    display->output_id = 0;
    int rc = prepare_handshake(display, MD_OP_HELLO);
    if (rc != MD_OK) return fail_session(display, (md_result_t)rc, "cannot encode hello");
    return MD_OK;
}

static int send_handshake(md_display_t* display) {
    const uint16_t minor = display->handshake_opcode == MD_OP_HELLO ? 0 : display->selected_minor;
    int rc = md_codec_send(display->fd, minor, display->handshake_opcode, 0,
                           display->handshake_serial, display->handshake_payload,
                           display->handshake_size, NULL, 0);
    if (rc == 1) return MD_HANDSHAKE_NEED_WRITE;
    if (rc < 0) return fail_session(display, map_io_error(rc), "handshake send failed");
    return MD_HANDSHAKE_PROGRESS;
}

static int receive_handshake(md_display_t* display, uint16_t expected, md_packet_t* packet) {
    int rc = md_codec_recv(display->fd, packet);
    if (rc == 0) return MD_HANDSHAKE_NEED_READ;
    if (rc < 0) return fail_session(display, map_io_error(rc), "handshake receive failed");
    if (packet->fd_count != 0 || packet->minor > MIRAGE_DISPLAY_PROTOCOL_MINOR) {
        md_packet_close_fds(packet);
        return fail_session(display, MD_ERR_PROTOCOL, "invalid handshake packet");
    }
    if (packet->opcode == MD_OP_ERROR) {
        md_proto_error_t error;
        if (md_proto_decode_error(packet->payload, packet->payload_size, &error) != 0) {
            return fail_session(display, MD_ERR_PROTOCOL, "malformed error packet");
        }
        int result = fail_session(display, MD_ERR_PROTOCOL,
                                  error.message != NULL ? error.message : "broker error");
        md_proto_error_clear(&error);
        return result;
    }
    if (packet->opcode != expected) {
        return fail_session(display, MD_ERR_PROTOCOL, "unexpected handshake opcode");
    }
    return MD_HANDSHAKE_PROGRESS;
}

md_display_t* md_display_new(const md_display_callbacks_t* callbacks) {
    md_display_t* display = calloc(1, sizeof(*display));
    if (display == NULL) return NULL;
    if (callbacks != NULL) display->callbacks = *callbacks;
    display->fd = -1;
    display->connection_state = MD_CONNECTION_DISCONNECTED;
    display->handshake_state = MD_HANDSHAKE_IDLE;
    display->next_serial = 1;
    init_pool(&display->pool);
    return display;
}

void md_display_free(md_display_t* display) {
    if (display == NULL) return;
    md_display_close(display);
    free_connection_data(display);
    free(display);
}

int md_display_begin_connect(md_display_t* display, const char* socket_path,
                             const char* client_name, const char* client_version,
                             const md_output_info_t* output, const md_consumer_caps_t* caps) {
    if (display == NULL || display->connection_state != MD_CONNECTION_DISCONNECTED) {
        return MD_ERR_STATE;
    }
    int rc = copy_connect_args(display, socket_path, client_name, client_version, output, caps);
    if (rc != MD_OK) return rc;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return MD_ERR_IO;
    struct sockaddr_un address;
    socklen_t address_length;
    rc = fill_unix_address(socket_path, &address, &address_length);
    if (rc != MD_OK) {
        close(fd);
        return rc;
    }

    display->fd = fd;
    display->connection_state = MD_CONNECTION_CONNECTING;
    display->handshake_state = MD_HANDSHAKE_CONNECTING;
    display->disconnected_notified = false;
    display->selected_minor = 0;
    display->negotiated_features = 0;
    display->output_id = 0;

    if (connect(fd, (struct sockaddr*)&address, address_length) == 0) {
        rc = start_connected_fd(display, fd);
        if (rc != MD_OK) return rc;
    } else if (errno != EINPROGRESS && errno != EAGAIN && errno != EALREADY) {
        return fail_session(display, MD_ERR_IO, strerror(errno));
    }
    return MD_OK;
}

int md_display_begin_connected_fd(md_display_t* display, int connected_fd,
                                  const char* client_name, const char* client_version,
                                  const md_output_info_t* output,
                                  const md_consumer_caps_t* caps) {
    if (display == NULL || connected_fd < 0 ||
        display->connection_state != MD_CONNECTION_DISCONNECTED) return MD_ERR_STATE;
    int rc = copy_connect_args(display, "", client_name, client_version, output, caps);
    if (rc != MD_OK) return rc;
    return start_connected_fd(display, connected_fd);
}

int md_display_advance_handshake(md_display_t* display) {
    if (display == NULL || display->fd < 0) return MD_ERR_STATE;
    int rc;
    switch (display->handshake_state) {
    case MD_HANDSHAKE_CONNECTING: {
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(display->fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0) {
            return fail_session(display, MD_ERR_IO, "getsockopt(SO_ERROR) failed");
        }
        if (error == EINPROGRESS || error == EALREADY) return MD_HANDSHAKE_NEED_WRITE;
        if (error != 0) return fail_session(display, MD_ERR_IO, strerror(error));
        rc = prepare_handshake(display, MD_OP_HELLO);
        if (rc != MD_OK) return fail_session(display, (md_result_t)rc, "cannot encode hello");
        display->connection_state = MD_CONNECTION_HANDSHAKING;
        display->handshake_state = MD_HANDSHAKE_HELLO_SEND;
        return MD_HANDSHAKE_PROGRESS;
    }
    case MD_HANDSHAKE_HELLO_SEND:
        rc = send_handshake(display);
        if (rc == MD_HANDSHAKE_PROGRESS) display->handshake_state = MD_HANDSHAKE_WELCOME_WAIT;
        return rc;
    case MD_HANDSHAKE_WELCOME_WAIT: {
        md_packet_t packet;
        rc = receive_handshake(display, MD_OP_WELCOME, &packet);
        if (rc != MD_HANDSHAKE_PROGRESS) return rc;
        md_proto_welcome_t welcome;
        if (md_proto_decode_welcome(packet.payload, packet.payload_size, &welcome) != 0 ||
            welcome.selected_minor > MIRAGE_DISPLAY_PROTOCOL_MINOR ||
            (welcome.features & MD_FEATURE_EXPLICIT_SYNC) == 0) {
            return fail_session(display, MD_ERR_PROTOCOL, "unsupported welcome packet");
        }
        display->selected_minor = welcome.selected_minor;
        display->negotiated_features = welcome.features & display->caps.features;
        md_proto_welcome_clear(&welcome);
        rc = prepare_handshake(display, MD_OP_REGISTER_OUTPUT);
        if (rc != MD_OK) return fail_session(display, (md_result_t)rc, "cannot encode output");
        display->handshake_state = MD_HANDSHAKE_REGISTER_SEND;
        return MD_HANDSHAKE_PROGRESS;
    }
    case MD_HANDSHAKE_REGISTER_SEND:
        rc = send_handshake(display);
        if (rc == MD_HANDSHAKE_PROGRESS) display->handshake_state = MD_HANDSHAKE_ACCEPT_WAIT;
        return rc;
    case MD_HANDSHAKE_ACCEPT_WAIT: {
        md_packet_t packet;
        rc = receive_handshake(display, MD_OP_OUTPUT_ACCEPTED, &packet);
        if (rc != MD_HANDSHAKE_PROGRESS) return rc;
        if (md_proto_decode_output_accepted(packet.payload, packet.payload_size,
                                            &display->output_id) != 0 || display->output_id == 0) {
            return fail_session(display, MD_ERR_PROTOCOL, "malformed output accepted packet");
        }
        rc = prepare_handshake(display, MD_OP_CONSUMER_CAPS);
        if (rc != MD_OK) return fail_session(display, (md_result_t)rc, "cannot encode caps");
        display->handshake_state = MD_HANDSHAKE_CAPS_SEND;
        return MD_HANDSHAKE_PROGRESS;
    }
    case MD_HANDSHAKE_CAPS_SEND:
        rc = send_handshake(display);
        if (rc != MD_HANDSHAKE_PROGRESS) return rc;
        display->handshake_state = MD_HANDSHAKE_READY;
        display->connection_state = MD_CONNECTION_READY;
        if (display->callbacks.on_connected != NULL) {
            display->callbacks.on_connected(display->callbacks.user_data, display->output_id);
        }
        return MD_HANDSHAKE_DONE;
    case MD_HANDSHAKE_READY:
        return MD_HANDSHAKE_DONE;
    case MD_HANDSHAKE_IDLE:
    default:
        return MD_ERR_STATE;
    }
}

static int64_t monotonic_millis(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return -1;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

int md_display_connect(md_display_t* display, const char* socket_path,
                       const char* client_name, const char* client_version,
                       const md_output_info_t* output, const md_consumer_caps_t* caps,
                       int timeout_ms) {
    int rc = md_display_begin_connect(display, socket_path, client_name, client_version,
                                      output, caps);
    if (rc != MD_OK) return rc;
    int64_t start = monotonic_millis();
    for (;;) {
        rc = md_display_advance_handshake(display);
        if (rc == MD_HANDSHAKE_DONE) return MD_OK;
        if (rc < 0) return rc;
        short events = rc == MD_HANDSHAKE_NEED_WRITE ? POLLOUT : POLLIN;
        if (rc == MD_HANDSHAKE_PROGRESS) continue;
        int wait_ms = timeout_ms;
        if (timeout_ms >= 0 && start >= 0) {
            int64_t elapsed = monotonic_millis() - start;
            if (elapsed >= timeout_ms) {
                md_display_close(display);
                return MD_ERR_IO;
            }
            wait_ms = timeout_ms - (int)elapsed;
        }
        struct pollfd pfd = {.fd = display->fd, .events = events, .revents = 0};
        int poll_rc;
        do { poll_rc = poll(&pfd, 1, wait_ms); } while (poll_rc < 0 && errno == EINTR);
        if (poll_rc <= 0) {
            md_display_close(display);
            return MD_ERR_IO;
        }
    }
}

void md_display_close(md_display_t* display) {
    if (display == NULL) return;
    if (display->fd >= 0) {
        uint8_t payload[4];
        md_writer_t writer; md_writer_init(&writer, payload, sizeof(payload));
        if (display->connection_state == MD_CONNECTION_READY &&
            md_proto_encode_u32(&writer, 0) == 0) {
            (void)md_codec_send(display->fd, display->selected_minor, MD_OP_GOODBYE, 0,
                                display->next_serial++, payload, writer.size, NULL, 0);
        }
        close(display->fd);
    }
    display->fd = -1;
    abandon_pool(display, true);
    clear_outbox(display);
    display->connection_state = MD_CONNECTION_DISCONNECTED;
    display->handshake_state = MD_HANDSHAKE_IDLE;
    display->output_id = 0;
    display->selected_minor = 0;
    display->negotiated_features = 0;
    display->disconnected_notified = false;
}

int md_display_get_fd(const md_display_t* display) { return display != NULL ? display->fd : -1; }
md_connection_state_t md_display_connection_state(const md_display_t* display) {
    return display != NULL ? display->connection_state : MD_CONNECTION_DEAD;
}
md_handshake_state_t md_display_handshake_state(const md_display_t* display) {
    return display != NULL ? display->handshake_state : MD_HANDSHAKE_IDLE;
}
uint64_t md_display_output_id(const md_display_t* display) {
    return display != NULL ? display->output_id : 0;
}

static bool coalescible(uint16_t opcode) {
    return opcode == MD_OP_POINTER_MOTION || opcode == MD_OP_UPDATE_OUTPUT ||
           opcode == MD_OP_WINDOW_STATE;
}

static int flush_outbox(md_display_t* display) {
    while (display->out_head != NULL) {
        md_out_message_t* message = display->out_head;
        int rc = md_codec_send(display->fd, display->selected_minor, message->opcode,
                               message->flags, message->serial, message->payload,
                               message->size, NULL, 0);
        if (rc == 1) return MD_ERR_WOULD_BLOCK;
        if (rc < 0) return fail_session(display, map_io_error(rc), "outbox send failed");
        display->out_head = message->next;
        if (display->out_head == NULL) display->out_tail = NULL;
        --display->out_count;
        free(message);
    }
    return MD_OK;
}

static int queue_message(md_display_t* display, uint16_t opcode, uint16_t flags,
                         const uint8_t* payload, size_t size) {
    if (display == NULL || display->connection_state != MD_CONNECTION_READY) return MD_ERR_STATE;
    if (display->out_head == NULL) {
        uint32_t serial = display->next_serial++;
        int rc = md_codec_send(display->fd, display->selected_minor, opcode, flags, serial,
                               payload, size, NULL, 0);
        if (rc == 0) return MD_OK;
        if (rc < 0) return fail_session(display, map_io_error(rc), "request send failed");
        md_out_message_t* message = malloc(sizeof(*message) + size);
        if (message == NULL) return MD_ERR_NOMEM;
        message->next = NULL; message->opcode = opcode; message->flags = flags;
        message->serial = serial; message->size = size;
        if (size > 0) memcpy(message->payload, payload, size);
        display->out_head = message; display->out_tail = message; display->out_count = 1;
        return MD_OK;
    }

    if (coalescible(opcode) && display->out_tail->opcode == opcode &&
        display->out_tail->size == size) {
        if (size > 0) memcpy(display->out_tail->payload, payload, size);
        return MD_OK;
    }
    if (display->out_count >= MD_OUTBOX_LIMIT) return MD_ERR_WOULD_BLOCK;
    md_out_message_t* message = malloc(sizeof(*message) + size);
    if (message == NULL) return MD_ERR_NOMEM;
    message->next = NULL; message->opcode = opcode; message->flags = flags;
    message->serial = display->next_serial++; message->size = size;
    if (size > 0) memcpy(message->payload, payload, size);
    display->out_tail->next = message; display->out_tail = message; ++display->out_count;
    return MD_OK;
}

bool md_display_wants_writable(const md_display_t* display) {
    return display != NULL && display->out_head != NULL;
}

int md_display_handle_writable(md_display_t* display) {
    if (display == NULL || display->connection_state != MD_CONNECTION_READY) return MD_ERR_STATE;
    int rc = flush_outbox(display);
    return rc == MD_ERR_WOULD_BLOCK ? MD_OK : rc;
}

int md_display_defer_unbind(md_display_t* display) {
    if (display == NULL) return MD_ERR_INVALID;
    if (!display->in_unbind_callback || display->pending_unbind_generation == 0) {
        return MD_ERR_STATE;
    }
    display->unbind_deferred = true;
    return MD_OK;
}

uint64_t md_display_pending_unbind_generation(const md_display_t* display) {
    if (display == NULL || !display->unbind_deferred) return 0;
    return display->pending_unbind_generation;
}

static int complete_unbind(md_display_t* display, uint64_t generation) {
    if (display == NULL || generation == 0 || !display->pool_active ||
        display->pending_unbind_generation != generation) {
        return MD_ERR_STATE;
    }
    release_pool(display, false);
    display->in_unbind_callback = false;
    display->unbind_deferred = false;
    display->pending_unbind_generation = 0;

    uint8_t payload[8];
    md_writer_t writer;
    md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_u64(&writer, generation) != 0) return MD_ERR_PROTOCOL;
    return queue_message(display, MD_OP_UNBIND_DONE, 0, payload, writer.size);
}

int md_display_finish_unbind(md_display_t* display, uint64_t generation) {
    if (display == NULL) return MD_ERR_INVALID;
    if (!display->unbind_deferred) return MD_ERR_STATE;
    return complete_unbind(display, generation);
}

static int process_packet(md_display_t* display, md_packet_t* packet) {
    if (packet->major != MIRAGE_DISPLAY_PROTOCOL_MAJOR ||
        packet->minor != display->selected_minor) {
        return fail_session(display, MD_ERR_PROTOCOL, "wire version changed during session");
    }
    if (packet->opcode == MD_OP_ERROR) {
        if (packet->fd_count != 0) return fail_session(display, MD_ERR_PROTOCOL, "error has FDs");
        md_proto_error_t error;
        if (md_proto_decode_error(packet->payload, packet->payload_size, &error) != 0) {
            return fail_session(display, MD_ERR_PROTOCOL, "malformed error packet");
        }
        md_result_t reason = error.fatal ? MD_ERR_PROTOCOL : MD_ERR_IO;
        int rc = error.fatal ? fail_session(display, reason, error.message) : MD_OK;
        md_proto_error_clear(&error);
        return rc;
    }

    if (display->pending_unbind_generation != 0) {
        return fail_session(display, MD_ERR_PROTOCOL,
                            "packet received before deferred unbind completed");
    }

    switch (packet->opcode) {
    case MD_OP_BIND_BUFFERS: {
        if (display->pool_active) return fail_session(display, MD_ERR_PROTOCOL, "pool rebound without unbind");
        md_buffer_pool_t pool;
        if (md_proto_decode_bind_buffers(packet->payload, packet->payload_size, &pool) != 0) {
            return fail_session(display, MD_ERR_PROTOCOL, "malformed buffer pool");
        }
        size_t expected = (size_t)pool.buffer_count * (size_t)pool.plane_count;
        if (packet->fd_count != expected || pool.generation == 0) {
            return fail_session(display, MD_ERR_PROTOCOL, "buffer pool FD count mismatch");
        }
        size_t index = 0;
        for (uint32_t b = 0; b < pool.buffer_count; ++b) {
            for (uint32_t p = 0; p < pool.plane_count; ++p) {
                pool.planes[b][p].fd = packet->fds[index];
                packet->fds[index++] = -1;
            }
        }
        packet->fd_count = 0;
        display->pool = pool;
        display->pool_active = true;
        if (display->callbacks.on_buffers_ready != NULL) {
            display->callbacks.on_buffers_ready(display->callbacks.user_data, &display->pool);
        }
        return MD_OK;
    }
    case MD_OP_SET_CONFIG: {
        if (packet->fd_count != 0) return fail_session(display, MD_ERR_PROTOCOL, "config has FDs");
        md_display_config_t config;
        if (md_proto_decode_config(packet->payload, packet->payload_size, &config) != 0) {
            return fail_session(display, MD_ERR_PROTOCOL, "malformed display config");
        }
        if (display->callbacks.on_config != NULL) {
            display->callbacks.on_config(display->callbacks.user_data, &config);
        }
        return MD_OK;
    }
    case MD_OP_FRAME_READY: {
        if (packet->fd_count != 2) {
            if (packet->fd_count >= 1 && packet->fds[0] >= 0) {
                close(packet->fds[0]);
                packet->fds[0] = -1;
            }
            if (packet->fd_count >= 2 && packet->fds[1] >= 0) {
                (void)md_display_signal_release_syncobj_on_node(
                    packet->fds[1], display->output.drm_render_major,
                    display->output.drm_render_minor);
                packet->fds[1] = -1;
            }
            return fail_session(display, MD_ERR_PROTOCOL, "frame FD count mismatch");
        }
        md_frame_t frame;
        if (md_proto_decode_frame(packet->payload, packet->payload_size, &frame) != 0) {
            close(packet->fds[0]);
            packet->fds[0] = -1;
            (void)md_display_signal_release_syncobj_on_node(
                packet->fds[1], display->output.drm_render_major,
                display->output.drm_render_minor);
            packet->fds[1] = -1;
            return fail_session(display, MD_ERR_PROTOCOL, "malformed frame");
        }
        if (!display->pool_active || frame.buffer_generation != display->pool.generation) {
            close(packet->fds[0]);
            packet->fds[0] = -1;
            (void)md_display_signal_release_syncobj_on_node(
                packet->fds[1], display->output.drm_render_major,
                display->output.drm_render_minor);
            packet->fds[1] = -1;
            return MD_OK;
        }
        if (frame.buffer_index >= display->pool.buffer_count) {
            close(packet->fds[0]);
            packet->fds[0] = -1;
            (void)md_display_signal_release_syncobj_on_node(
                packet->fds[1], display->output.drm_render_major,
                display->output.drm_render_minor);
            packet->fds[1] = -1;
            return fail_session(display, MD_ERR_PROTOCOL, "frame buffer index out of range");
        }
        frame.acquire_sync_fd = packet->fds[0];
        frame.release_syncobj_fd = packet->fds[1];
        packet->fds[0] = -1; packet->fds[1] = -1; packet->fd_count = 0;
        if (display->callbacks.on_frame != NULL) {
            display->callbacks.on_frame(display->callbacks.user_data, &frame);
        } else {
            close(frame.acquire_sync_fd);
            close(frame.release_syncobj_fd);
        }
        return MD_OK;
    }
    case MD_OP_UNBIND: {
        if (packet->fd_count != 0) return fail_session(display, MD_ERR_PROTOCOL, "unbind has FDs");
        uint64_t generation;
        if (md_proto_decode_unbind(packet->payload, packet->payload_size, &generation) != 0 ||
            !display->pool_active || generation != display->pool.generation) {
            return fail_session(display, MD_ERR_PROTOCOL, "invalid unbind generation");
        }
        if (display->pending_unbind_generation != 0) {
            return fail_session(display, MD_ERR_PROTOCOL, "overlapping unbind");
        }
        display->pending_unbind_generation = generation;
        display->in_unbind_callback = true;
        display->unbind_deferred = false;
        if (display->callbacks.on_buffers_releasing != NULL) {
            display->callbacks.on_buffers_releasing(display->callbacks.user_data, &display->pool);
        }
        display->in_unbind_callback = false;
        if (display->unbind_deferred) return MD_OK;
        return complete_unbind(display, generation);
    }
    default:
        if ((packet->flags & MD_PACKET_OPTIONAL) != 0) return MD_OK;
        return fail_session(display, MD_ERR_PROTOCOL, "unknown required opcode");
    }
}

int md_display_dispatch(md_display_t* display) {
    if (display == NULL || display->connection_state != MD_CONNECTION_READY) return MD_ERR_STATE;
    int count = 0;
    for (;;) {
        md_packet_t packet;
        int rc = md_codec_recv(display->fd, &packet);
        if (rc == 0) break;
        if (rc < 0) return fail_session(display, map_io_error(rc), "session receive failed");
        rc = process_packet(display, &packet);
        md_packet_close_fds(&packet);
        if (rc < 0) return rc;
        ++count;
    }
    int rc = flush_outbox(display);
    if (rc < 0 && rc != MD_ERR_WOULD_BLOCK) return rc;
    return count;
}

static int encode_and_queue(md_display_t* display, uint16_t opcode,
                            int (*encode)(md_writer_t*, void*), void* context) {
    uint8_t payload[128];
    md_writer_t writer; md_writer_init(&writer, payload, sizeof(payload));
    int rc = encode(&writer, context);
    if (rc != 0) return MD_ERR_INVALID;
    return queue_message(display, opcode, 0, payload, writer.size);
}

typedef struct md_motion_args { float x, y; uint64_t timestamp; uint32_t modifiers; } md_motion_args_t;
typedef struct md_button_args { float x, y; uint32_t button; md_button_state_t state; uint64_t timestamp; uint32_t modifiers; } md_button_args_t;
typedef struct md_axis_args { float x, y, dx, dy; md_axis_source_t source; uint64_t timestamp; uint32_t modifiers; } md_axis_args_t;

static int encode_update(md_writer_t* writer, void* context) { return md_proto_encode_update_output(writer, context); }
static int encode_enter(md_writer_t* writer, void* context) {
    md_motion_args_t* args = context; return md_proto_encode_pointer_enter(writer, args->x, args->y, args->timestamp);
}
static int encode_leave(md_writer_t* writer, void* context) { return md_proto_encode_pointer_leave(writer, *(uint64_t*)context); }
static int encode_motion(md_writer_t* writer, void* context) {
    md_motion_args_t* args = context; return md_proto_encode_pointer_motion(writer, args->x, args->y, args->timestamp, args->modifiers);
}
static int encode_button(md_writer_t* writer, void* context) {
    md_button_args_t* args = context; return md_proto_encode_pointer_button(writer, args->x, args->y, args->button, args->state, args->timestamp, args->modifiers);
}
static int encode_axis(md_writer_t* writer, void* context) {
    md_axis_args_t* args = context; return md_proto_encode_pointer_axis(writer, args->x, args->y, args->dx, args->dy, args->source, args->timestamp, args->modifiers);
}
static int encode_u32_context(md_writer_t* writer, void* context) { return md_proto_encode_u32(writer, *(uint32_t*)context); }

int md_display_update_output(md_display_t* display, const md_output_info_t* output) {
    if (!valid_output(output)) return MD_ERR_INVALID;
    return encode_and_queue(display, MD_OP_UPDATE_OUTPUT, encode_update, (void*)output);
}

int md_display_send_pointer_enter(md_display_t* display, float x, float y, uint64_t timestamp_us) {
    md_motion_args_t args = {.x = x, .y = y, .timestamp = timestamp_us, .modifiers = 0};
    return encode_and_queue(display, MD_OP_POINTER_ENTER, encode_enter, &args);
}

int md_display_send_pointer_leave(md_display_t* display, uint64_t timestamp_us) {
    return encode_and_queue(display, MD_OP_POINTER_LEAVE, encode_leave, &timestamp_us);
}

int md_display_send_pointer_motion(md_display_t* display, float x, float y,
                                   uint64_t timestamp_us, uint32_t modifiers) {
    md_motion_args_t args = {.x = x, .y = y, .timestamp = timestamp_us, .modifiers = modifiers};
    return encode_and_queue(display, MD_OP_POINTER_MOTION, encode_motion, &args);
}

int md_display_send_pointer_button(md_display_t* display, float x, float y, uint32_t button,
                                   md_button_state_t state, uint64_t timestamp_us,
                                   uint32_t modifiers) {
    if (state != MD_BUTTON_RELEASED && state != MD_BUTTON_PRESSED) return MD_ERR_INVALID;
    md_button_args_t args = {.x = x, .y = y, .button = button, .state = state,
                             .timestamp = timestamp_us, .modifiers = modifiers};
    return encode_and_queue(display, MD_OP_POINTER_BUTTON, encode_button, &args);
}

int md_display_send_pointer_axis(md_display_t* display, float x, float y, float delta_x,
                                 float delta_y, md_axis_source_t source, uint64_t timestamp_us,
                                 uint32_t modifiers) {
    if (source < MD_AXIS_WHEEL || source > MD_AXIS_CONTINUOUS) return MD_ERR_INVALID;
    md_axis_args_t args = {.x = x, .y = y, .dx = delta_x, .dy = delta_y, .source = source,
                           .timestamp = timestamp_us, .modifiers = modifiers};
    return encode_and_queue(display, MD_OP_POINTER_AXIS, encode_axis, &args);
}

int md_display_send_window_state(md_display_t* display, uint32_t flags) {
    return encode_and_queue(display, MD_OP_WINDOW_STATE, encode_u32_context, &flags);
}
