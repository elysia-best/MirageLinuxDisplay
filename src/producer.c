#define _GNU_SOURCE

#include "mirage_display_producer.h"

#include "codec.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MD_PRODUCER_OUTBOX_LIMIT 64u

typedef struct md_producer_out_message {
    struct md_producer_out_message* next;
    uint16_t opcode;
    uint32_t serial;
    size_t payload_size;
    size_t fd_count;
    int fds[MD_WIRE_MAX_FDS];
    uint8_t payload[];
} md_producer_out_message_t;

struct md_producer {
    md_producer_callbacks_t callbacks;
    int fd;
    md_connection_state_t connection_state;
    md_handshake_state_t handshake_state;
    uint16_t selected_minor;
    uint64_t features;
    uint64_t producer_id;
    uint64_t output_id;
    uint32_t next_serial;
    bool disconnected_notified;

    char* socket_path;
    char* client_name;
    char* client_version;
    char* stable_output_id;
    char* kind;
    md_producer_info_t info;
    md_format_cap_t* formats;

    uint16_t handshake_opcode;
    uint32_t handshake_serial;
    size_t handshake_size;
    uint8_t handshake_payload[MD_WIRE_MAX_PAYLOAD];

    md_producer_out_message_t* out_head;
    md_producer_out_message_t* out_tail;
    size_t out_count;

    bool pool_offered;
    bool retire_pending;
    uint64_t pool_generation;
    uint32_t pool_buffer_count;
};

static char* duplicate_string(const char* value) {
    if (value == NULL) return NULL;
    size_t size = strlen(value) + 1u;
    char* copy = malloc(size);
    if (copy != NULL) memcpy(copy, value, size);
    return copy;
}

static void close_fds(int* fds, size_t count) {
    if (fds == NULL) return;
    for (size_t i = 0; i < count; ++i) {
        if (fds[i] >= 0) close(fds[i]);
        fds[i] = -1;
    }
}

static void clear_outbox(md_producer_t* producer) {
    md_producer_out_message_t* message = producer->out_head;
    while (message != NULL) {
        md_producer_out_message_t* next = message->next;
        close_fds(message->fds, message->fd_count);
        free(message);
        message = next;
    }
    producer->out_head = NULL;
    producer->out_tail = NULL;
    producer->out_count = 0;
}

static void clear_connect_args(md_producer_t* producer) {
    free(producer->socket_path);
    free(producer->client_name);
    free(producer->client_version);
    free(producer->stable_output_id);
    free(producer->kind);
    free(producer->formats);
    producer->socket_path = NULL;
    producer->client_name = NULL;
    producer->client_version = NULL;
    producer->stable_output_id = NULL;
    producer->kind = NULL;
    producer->formats = NULL;
    memset(&producer->info, 0, sizeof(producer->info));
}

static md_result_t map_io_error(int error) {
    if (error == -ENOMEM) return MD_ERR_NOMEM;
    if (error == -EPROTO || error == -EMSGSIZE) return MD_ERR_PROTOCOL;
    if (error == -ECONNRESET || error == -EPIPE || error == -ENOTCONN) {
        return MD_ERR_DISCONNECTED;
    }
    return MD_ERR_IO;
}

static int fail_producer(md_producer_t* producer, md_result_t reason, const char* message) {
    if (producer->fd >= 0) close(producer->fd);
    producer->fd = -1;
    clear_outbox(producer);
    producer->connection_state = MD_CONNECTION_DEAD;
    producer->handshake_state = MD_HANDSHAKE_IDLE;
    producer->pool_offered = false;
    producer->retire_pending = false;
    producer->pool_generation = 0;
    producer->pool_buffer_count = 0;
    if (!producer->disconnected_notified && producer->callbacks.on_disconnected != NULL) {
        producer->disconnected_notified = true;
        producer->callbacks.on_disconnected(producer->callbacks.user_data, reason,
                                            message != NULL ? message : "producer failed");
    }
    return reason;
}

static bool valid_info(const md_producer_info_t* info) {
    if (info == NULL || info->stable_output_id == NULL || info->kind == NULL ||
        info->format_count == 0 || info->format_count > MIRAGE_DISPLAY_MAX_FORMATS ||
        info->formats == NULL) return false;
    for (uint32_t i = 0; i < info->format_count; ++i) {
        if (info->formats[i].plane_count == 0 ||
            info->formats[i].plane_count > MIRAGE_DISPLAY_MAX_PLANES) return false;
    }
    return true;
}

static int copy_connect_args(md_producer_t* producer, const char* socket_path,
                             const char* client_name, const char* client_version,
                             const md_producer_info_t* info) {
    if (socket_path == NULL || client_name == NULL || client_version == NULL ||
        !valid_info(info)) return MD_ERR_INVALID;
    clear_connect_args(producer);
    producer->socket_path = duplicate_string(socket_path);
    producer->client_name = duplicate_string(client_name);
    producer->client_version = duplicate_string(client_version);
    producer->stable_output_id = duplicate_string(info->stable_output_id);
    producer->kind = duplicate_string(info->kind);
    if (producer->socket_path == NULL || producer->client_name == NULL ||
        producer->client_version == NULL || producer->stable_output_id == NULL ||
        producer->kind == NULL) {
        clear_connect_args(producer);
        return MD_ERR_NOMEM;
    }
    producer->formats = malloc(sizeof(*producer->formats) * info->format_count);
    if (producer->formats == NULL) {
        clear_connect_args(producer);
        return MD_ERR_NOMEM;
    }
    memcpy(producer->formats, info->formats, sizeof(*producer->formats) * info->format_count);
    producer->info = *info;
    producer->info.stable_output_id = producer->stable_output_id;
    producer->info.kind = producer->kind;
    producer->info.formats = producer->formats;
    return MD_OK;
}

static int prepare_handshake(md_producer_t* producer, uint16_t opcode) {
    md_writer_t writer;
    md_writer_init(&writer, producer->handshake_payload, sizeof(producer->handshake_payload));
    int rc;
    switch (opcode) {
    case MD_OP_HELLO:
        rc = md_proto_encode_hello(&writer, 2, producer->client_name,
                                   producer->client_version, producer->features);
        break;
    case MD_OP_REGISTER_PRODUCER:
        rc = md_proto_encode_register_producer(&writer, &producer->info);
        break;
    default:
        return MD_ERR_INVALID;
    }
    if (rc != 0) return rc == -ENOMEM ? MD_ERR_NOMEM : MD_ERR_INVALID;
    producer->handshake_opcode = opcode;
    producer->handshake_serial = producer->next_serial++;
    producer->handshake_size = writer.size;
    return MD_OK;
}

static int start_connected_fd(md_producer_t* producer, int fd) {
    int status_flags = fcntl(fd, F_GETFL);
    int descriptor_flags = fcntl(fd, F_GETFD);
    if (status_flags < 0 || descriptor_flags < 0 ||
        fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0 ||
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) return MD_ERR_IO;
    producer->fd = fd;
    producer->connection_state = MD_CONNECTION_HANDSHAKING;
    producer->handshake_state = MD_HANDSHAKE_HELLO_SEND;
    producer->disconnected_notified = false;
    producer->selected_minor = 0;
    producer->producer_id = 0;
    producer->output_id = 0;
    int rc = prepare_handshake(producer, MD_OP_HELLO);
    return rc == MD_OK ? MD_OK
                       : fail_producer(producer, (md_result_t)rc, "cannot encode hello");
}

static int send_handshake(md_producer_t* producer) {
    uint16_t minor = producer->handshake_opcode == MD_OP_HELLO ? 0 : producer->selected_minor;
    int rc = md_codec_send(producer->fd, minor, producer->handshake_opcode, 0,
                           producer->handshake_serial, producer->handshake_payload,
                           producer->handshake_size, NULL, 0);
    if (rc == 1) return MD_HANDSHAKE_NEED_WRITE;
    if (rc < 0) return fail_producer(producer, map_io_error(rc), "handshake send failed");
    return MD_HANDSHAKE_PROGRESS;
}

static int receive_handshake(md_producer_t* producer, uint16_t expected, md_packet_t* packet) {
    int rc = md_codec_recv(producer->fd, packet);
    if (rc == 0) return MD_HANDSHAKE_NEED_READ;
    if (rc < 0) return fail_producer(producer, map_io_error(rc), "handshake receive failed");
    if (packet->fd_count != 0 || packet->minor > MIRAGE_DISPLAY_PROTOCOL_MINOR) {
        md_packet_close_fds(packet);
        return fail_producer(producer, MD_ERR_PROTOCOL, "invalid handshake packet");
    }
    if (packet->opcode == MD_OP_ERROR) {
        md_proto_error_t error;
        if (md_proto_decode_error(packet->payload, packet->payload_size, &error) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed broker error");
        }
        int result = fail_producer(producer, MD_ERR_PROTOCOL, error.message);
        md_proto_error_clear(&error);
        return result;
    }
    if (packet->opcode != expected) {
        return fail_producer(producer, MD_ERR_PROTOCOL, "unexpected handshake opcode");
    }
    return MD_HANDSHAKE_PROGRESS;
}

md_producer_t* md_producer_new(const md_producer_callbacks_t* callbacks) {
    md_producer_t* producer = calloc(1, sizeof(*producer));
    if (producer == NULL) return NULL;
    if (callbacks != NULL) producer->callbacks = *callbacks;
    producer->fd = -1;
    producer->connection_state = MD_CONNECTION_DISCONNECTED;
    producer->handshake_state = MD_HANDSHAKE_IDLE;
    producer->next_serial = 1;
    producer->features = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS |
                         MD_FEATURE_MULTIPLANE | MD_FEATURE_POINTER_AXIS;
    return producer;
}

void md_producer_free(md_producer_t* producer) {
    if (producer == NULL) return;
    md_producer_close(producer);
    clear_connect_args(producer);
    free(producer);
}

int md_producer_begin_connect(md_producer_t* producer, const char* socket_path,
                              const char* client_name, const char* client_version,
                              const md_producer_info_t* info) {
    if (producer == NULL || producer->connection_state != MD_CONNECTION_DISCONNECTED) {
        return MD_ERR_STATE;
    }
    int rc = copy_connect_args(producer, socket_path, client_name, client_version, info);
    if (rc != MD_OK) return rc;
    if (strlen(socket_path) >= sizeof(((struct sockaddr_un*)0)->sun_path)) return MD_ERR_INVALID;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return MD_ERR_IO;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1u);
    producer->fd = fd;
    producer->connection_state = MD_CONNECTION_CONNECTING;
    producer->handshake_state = MD_HANDSHAKE_CONNECTING;
    producer->disconnected_notified = false;
    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) == 0) {
        return start_connected_fd(producer, fd);
    }
    if (errno != EINPROGRESS && errno != EAGAIN && errno != EALREADY) {
        return fail_producer(producer, MD_ERR_IO, strerror(errno));
    }
    return MD_OK;
}

int md_producer_begin_connected_fd(md_producer_t* producer, int connected_fd,
                                   const char* client_name, const char* client_version,
                                   const md_producer_info_t* info) {
    if (producer == NULL || connected_fd < 0 ||
        producer->connection_state != MD_CONNECTION_DISCONNECTED) return MD_ERR_STATE;
    int rc = copy_connect_args(producer, "", client_name, client_version, info);
    if (rc != MD_OK) return rc;
    return start_connected_fd(producer, connected_fd);
}

int md_producer_advance_handshake(md_producer_t* producer) {
    if (producer == NULL || producer->fd < 0) return MD_ERR_STATE;
    int rc;
    switch (producer->handshake_state) {
    case MD_HANDSHAKE_CONNECTING: {
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(producer->fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0) {
            return fail_producer(producer, MD_ERR_IO, "getsockopt(SO_ERROR) failed");
        }
        if (error == EINPROGRESS || error == EALREADY) return MD_HANDSHAKE_NEED_WRITE;
        if (error != 0) return fail_producer(producer, MD_ERR_IO, strerror(error));
        rc = prepare_handshake(producer, MD_OP_HELLO);
        if (rc != MD_OK) return fail_producer(producer, (md_result_t)rc, "cannot encode hello");
        producer->connection_state = MD_CONNECTION_HANDSHAKING;
        producer->handshake_state = MD_HANDSHAKE_HELLO_SEND;
        return MD_HANDSHAKE_PROGRESS;
    }
    case MD_HANDSHAKE_HELLO_SEND:
        rc = send_handshake(producer);
        if (rc == MD_HANDSHAKE_PROGRESS) producer->handshake_state = MD_HANDSHAKE_WELCOME_WAIT;
        return rc;
    case MD_HANDSHAKE_WELCOME_WAIT: {
        md_packet_t packet;
        rc = receive_handshake(producer, MD_OP_WELCOME, &packet);
        if (rc != MD_HANDSHAKE_PROGRESS) return rc;
        md_proto_welcome_t welcome;
        if (md_proto_decode_welcome(packet.payload, packet.payload_size, &welcome) != 0 ||
            welcome.selected_minor > MIRAGE_DISPLAY_PROTOCOL_MINOR ||
            (welcome.features & MD_FEATURE_EXPLICIT_SYNC) == 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "unsupported welcome packet");
        }
        producer->selected_minor = welcome.selected_minor;
        producer->features &= welcome.features;
        md_proto_welcome_clear(&welcome);
        rc = prepare_handshake(producer, MD_OP_REGISTER_PRODUCER);
        if (rc != MD_OK) return fail_producer(producer, (md_result_t)rc, "cannot encode producer");
        producer->handshake_state = MD_HANDSHAKE_REGISTER_SEND;
        return MD_HANDSHAKE_PROGRESS;
    }
    case MD_HANDSHAKE_REGISTER_SEND:
        rc = send_handshake(producer);
        if (rc == MD_HANDSHAKE_PROGRESS) producer->handshake_state = MD_HANDSHAKE_ACCEPT_WAIT;
        return rc;
    case MD_HANDSHAKE_ACCEPT_WAIT: {
        md_packet_t packet;
        rc = receive_handshake(producer, MD_OP_PRODUCER_ACCEPTED, &packet);
        if (rc != MD_HANDSHAKE_PROGRESS) return rc;
        if (md_proto_decode_producer_accepted(packet.payload, packet.payload_size,
                                              &producer->producer_id,
                                              &producer->output_id) != 0 ||
            producer->producer_id == 0 || producer->output_id == 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed producer accepted packet");
        }
        producer->handshake_state = MD_HANDSHAKE_READY;
        producer->connection_state = MD_CONNECTION_READY;
        if (producer->callbacks.on_connected != NULL) {
            producer->callbacks.on_connected(producer->callbacks.user_data,
                                             producer->producer_id, producer->output_id);
        }
        return MD_HANDSHAKE_DONE;
    }
    case MD_HANDSHAKE_READY:
        return MD_HANDSHAKE_DONE;
    default:
        return MD_ERR_STATE;
    }
}

static int64_t monotonic_millis(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return -1;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

int md_producer_connect(md_producer_t* producer, const char* socket_path,
                        const char* client_name, const char* client_version,
                        const md_producer_info_t* info, int timeout_ms) {
    int rc = md_producer_begin_connect(producer, socket_path, client_name, client_version, info);
    if (rc != MD_OK) return rc;
    int64_t start = monotonic_millis();
    for (;;) {
        rc = md_producer_advance_handshake(producer);
        if (rc == MD_HANDSHAKE_DONE) return MD_OK;
        if (rc < 0) return rc;
        if (rc == MD_HANDSHAKE_PROGRESS) continue;
        short events = rc == MD_HANDSHAKE_NEED_WRITE ? POLLOUT : POLLIN;
        int wait_ms = timeout_ms;
        if (timeout_ms >= 0 && start >= 0) {
            int64_t elapsed = monotonic_millis() - start;
            if (elapsed >= timeout_ms) {
                md_producer_close(producer);
                return MD_ERR_IO;
            }
            wait_ms = timeout_ms - (int)elapsed;
        }
        struct pollfd pfd = {.fd = producer->fd, .events = events, .revents = 0};
        int poll_rc;
        do { poll_rc = poll(&pfd, 1, wait_ms); } while (poll_rc < 0 && errno == EINTR);
        if (poll_rc <= 0) {
            md_producer_close(producer);
            return MD_ERR_IO;
        }
    }
}

void md_producer_close(md_producer_t* producer) {
    if (producer == NULL) return;
    if (producer->fd >= 0) {
        uint8_t payload[4];
        md_writer_t writer; md_writer_init(&writer, payload, sizeof(payload));
        if (producer->connection_state == MD_CONNECTION_READY &&
            md_proto_encode_u32(&writer, 0) == 0) {
            (void)md_codec_send(producer->fd, producer->selected_minor, MD_OP_GOODBYE, 0,
                                producer->next_serial++, payload, writer.size, NULL, 0);
        }
        close(producer->fd);
    }
    producer->fd = -1;
    clear_outbox(producer);
    producer->connection_state = MD_CONNECTION_DISCONNECTED;
    producer->handshake_state = MD_HANDSHAKE_IDLE;
    producer->selected_minor = 0;
    producer->producer_id = 0;
    producer->output_id = 0;
    producer->pool_offered = false;
    producer->retire_pending = false;
    producer->pool_generation = 0;
    producer->pool_buffer_count = 0;
    producer->disconnected_notified = false;
}

int md_producer_get_fd(const md_producer_t* producer) { return producer != NULL ? producer->fd : -1; }
md_connection_state_t md_producer_connection_state(const md_producer_t* producer) {
    return producer != NULL ? producer->connection_state : MD_CONNECTION_DEAD;
}
md_handshake_state_t md_producer_handshake_state(const md_producer_t* producer) {
    return producer != NULL ? producer->handshake_state : MD_HANDSHAKE_IDLE;
}

static int flush_outbox(md_producer_t* producer) {
    while (producer->out_head != NULL) {
        md_producer_out_message_t* message = producer->out_head;
        int rc = md_codec_send(producer->fd, producer->selected_minor, message->opcode, 0,
                               message->serial, message->payload, message->payload_size,
                               message->fds, message->fd_count);
        if (rc == 1) return MD_ERR_WOULD_BLOCK;
        if (rc < 0) return fail_producer(producer, map_io_error(rc), "producer outbox failed");
        close_fds(message->fds, message->fd_count);
        producer->out_head = message->next;
        if (producer->out_head == NULL) producer->out_tail = NULL;
        --producer->out_count;
        free(message);
    }
    return MD_OK;
}

static int send_owned(md_producer_t* producer, uint16_t opcode,
                      const uint8_t* payload, size_t payload_size,
                      int* fds, size_t fd_count) {
    if (producer == NULL || producer->connection_state != MD_CONNECTION_READY) {
        close_fds(fds, fd_count);
        return MD_ERR_STATE;
    }
    if (fd_count > MD_WIRE_MAX_FDS) {
        close_fds(fds, fd_count);
        return MD_ERR_INVALID;
    }
    uint32_t serial = producer->next_serial++;
    if (producer->out_head == NULL) {
        int rc = md_codec_send(producer->fd, producer->selected_minor, opcode, 0, serial,
                               payload, payload_size, fds, fd_count);
        if (rc == 0) {
            close_fds(fds, fd_count);
            return MD_OK;
        }
        if (rc < 0) {
            close_fds(fds, fd_count);
            return fail_producer(producer, map_io_error(rc), "producer request failed");
        }
    }
    if (producer->out_count >= MD_PRODUCER_OUTBOX_LIMIT) {
        close_fds(fds, fd_count);
        return MD_ERR_WOULD_BLOCK;
    }
    md_producer_out_message_t* message = malloc(sizeof(*message) + payload_size);
    if (message == NULL) {
        close_fds(fds, fd_count);
        return MD_ERR_NOMEM;
    }
    message->next = NULL;
    message->opcode = opcode;
    message->serial = serial;
    message->payload_size = payload_size;
    message->fd_count = fd_count;
    for (size_t i = 0; i < MD_WIRE_MAX_FDS; ++i) message->fds[i] = -1;
    for (size_t i = 0; i < fd_count; ++i) {
        message->fds[i] = fds[i];
        fds[i] = -1;
    }
    if (payload_size > 0) memcpy(message->payload, payload, payload_size);
    if (producer->out_tail != NULL) producer->out_tail->next = message;
    else producer->out_head = message;
    producer->out_tail = message;
    ++producer->out_count;
    return MD_OK;
}

bool md_producer_wants_writable(const md_producer_t* producer) {
    return producer != NULL && producer->out_head != NULL;
}

int md_producer_handle_writable(md_producer_t* producer) {
    if (producer == NULL || producer->connection_state != MD_CONNECTION_READY) return MD_ERR_STATE;
    int rc = flush_outbox(producer);
    return rc == MD_ERR_WOULD_BLOCK ? MD_OK : rc;
}

static int process_packet(md_producer_t* producer, md_packet_t* packet) {
    if (packet->major != MIRAGE_DISPLAY_PROTOCOL_MAJOR ||
        packet->minor != producer->selected_minor) {
        return fail_producer(producer, MD_ERR_PROTOCOL, "producer wire version changed");
    }
    if (packet->fd_count != 0) {
        return fail_producer(producer, MD_ERR_PROTOCOL, "unexpected producer event FDs");
    }
    if (packet->opcode == MD_OP_ERROR) {
        md_proto_error_t error;
        if (md_proto_decode_error(packet->payload, packet->payload_size, &error) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed broker error");
        }
        int rc = error.fatal ? fail_producer(producer, MD_ERR_PROTOCOL, error.message) : MD_OK;
        md_proto_error_clear(&error);
        return rc;
    }
    switch (packet->opcode) {
    case MD_OP_OUTPUT_CONFIG: {
        md_producer_config_t config;
        if (md_proto_decode_output_config(packet->payload, packet->payload_size, &config) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed output config");
        }
        if (producer->callbacks.on_output_config != NULL) {
            producer->callbacks.on_output_config(producer->callbacks.user_data, &config);
        }
        return MD_OK;
    }
    case MD_OP_RETIRE_BUFFERS: {
        uint64_t generation;
        if (md_proto_decode_unbind(packet->payload, packet->payload_size, &generation) != 0 ||
            !producer->pool_offered || producer->retire_pending ||
            generation != producer->pool_generation) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "invalid retire generation");
        }
        producer->retire_pending = true;
        if (producer->callbacks.on_retire_buffers != NULL) {
            producer->callbacks.on_retire_buffers(producer->callbacks.user_data, generation);
        }
        return MD_OK;
    }
    case MD_OP_PRODUCER_POINTER_ENTER: {
        md_pointer_enter_t event;
        if (md_proto_decode_pointer_enter(packet->payload, packet->payload_size, &event) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed pointer enter");
        }
        if (producer->callbacks.on_pointer_enter != NULL) {
            producer->callbacks.on_pointer_enter(producer->callbacks.user_data, &event);
        }
        return MD_OK;
    }
    case MD_OP_PRODUCER_POINTER_LEAVE: {
        uint64_t timestamp;
        if (md_proto_decode_pointer_leave(packet->payload, packet->payload_size, &timestamp) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed pointer leave");
        }
        if (producer->callbacks.on_pointer_leave != NULL) {
            producer->callbacks.on_pointer_leave(producer->callbacks.user_data, timestamp);
        }
        return MD_OK;
    }
    case MD_OP_PRODUCER_POINTER_MOTION: {
        md_pointer_motion_t event;
        if (md_proto_decode_pointer_motion(packet->payload, packet->payload_size, &event) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed pointer motion");
        }
        if (producer->callbacks.on_pointer_motion != NULL) {
            producer->callbacks.on_pointer_motion(producer->callbacks.user_data, &event);
        }
        return MD_OK;
    }
    case MD_OP_PRODUCER_POINTER_BUTTON: {
        md_pointer_button_t event;
        if (md_proto_decode_pointer_button(packet->payload, packet->payload_size, &event) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed pointer button");
        }
        if (producer->callbacks.on_pointer_button != NULL) {
            producer->callbacks.on_pointer_button(producer->callbacks.user_data, &event);
        }
        return MD_OK;
    }
    case MD_OP_PRODUCER_POINTER_AXIS: {
        md_pointer_axis_t event;
        if (md_proto_decode_pointer_axis(packet->payload, packet->payload_size, &event) != 0) {
            return fail_producer(producer, MD_ERR_PROTOCOL, "malformed pointer axis");
        }
        if (producer->callbacks.on_pointer_axis != NULL) {
            producer->callbacks.on_pointer_axis(producer->callbacks.user_data, &event);
        }
        return MD_OK;
    }
    default:
        if ((packet->flags & MD_PACKET_OPTIONAL) != 0) return MD_OK;
        return fail_producer(producer, MD_ERR_PROTOCOL, "unknown required producer opcode");
    }
}

int md_producer_dispatch(md_producer_t* producer) {
    if (producer == NULL || producer->connection_state != MD_CONNECTION_READY) return MD_ERR_STATE;
    int count = 0;
    for (;;) {
        md_packet_t packet;
        int rc = md_codec_recv(producer->fd, &packet);
        if (rc == 0) break;
        if (rc < 0) return fail_producer(producer, map_io_error(rc), "producer receive failed");
        rc = process_packet(producer, &packet);
        md_packet_close_fds(&packet);
        if (rc < 0) return rc;
        ++count;
    }
    int rc = flush_outbox(producer);
    if (rc < 0 && rc != MD_ERR_WOULD_BLOCK) return rc;
    return count;
}

int md_producer_offer_buffers(md_producer_t* producer, const md_buffer_pool_t* pool) {
    if (producer == NULL || pool == NULL || producer->pool_offered) return MD_ERR_STATE;
    uint8_t payload[1024];
    md_writer_t writer; md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_offer_buffers(&writer, pool) != 0) return MD_ERR_INVALID;
    size_t fd_count = (size_t)pool->buffer_count * (size_t)pool->plane_count;
    if (fd_count > MD_WIRE_MAX_FDS) return MD_ERR_INVALID;
    int fds[MD_WIRE_MAX_FDS];
    for (size_t i = 0; i < MD_WIRE_MAX_FDS; ++i) fds[i] = -1;
    size_t index = 0;
    for (uint32_t b = 0; b < pool->buffer_count; ++b) {
        for (uint32_t p = 0; p < pool->plane_count; ++p) {
            if (pool->planes[b][p].fd < 0) {
                close_fds(fds, index);
                return MD_ERR_INVALID;
            }
            fds[index] = fcntl(pool->planes[b][p].fd, F_DUPFD_CLOEXEC, 0);
            if (fds[index] < 0) {
                close_fds(fds, index);
                return MD_ERR_IO;
            }
            ++index;
        }
    }
    int rc = send_owned(producer, MD_OP_OFFER_BUFFERS, payload, writer.size, fds, fd_count);
    if (rc == MD_OK) {
        producer->pool_offered = true;
        producer->pool_generation = pool->generation;
        producer->pool_buffer_count = pool->buffer_count;
    }
    return rc;
}

int md_producer_set_config(md_producer_t* producer, const md_display_config_t* config) {
    if (producer == NULL || config == NULL || !producer->pool_offered ||
        producer->retire_pending) return MD_ERR_STATE;
    uint8_t payload[128];
    md_writer_t writer; md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_config(&writer, config) != 0) return MD_ERR_INVALID;
    return send_owned(producer, MD_OP_PRODUCER_SET_CONFIG, payload, writer.size, NULL, 0);
}

int md_producer_submit_frame(md_producer_t* producer, uint64_t generation,
                             uint32_t buffer_index, uint64_t sequence,
                             int acquire_sync_fd, int release_syncobj_fd) {
    int fds[2] = {acquire_sync_fd, release_syncobj_fd};
    if (producer == NULL || acquire_sync_fd < 0 || release_syncobj_fd < 0 ||
        !producer->pool_offered || producer->retire_pending ||
        generation != producer->pool_generation ||
        buffer_index >= producer->pool_buffer_count) {
        close_fds(fds, 2);
        return MD_ERR_STATE;
    }
    uint8_t payload[32];
    md_writer_t writer; md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_producer_frame(&writer, generation, buffer_index, sequence) != 0) {
        close_fds(fds, 2);
        return MD_ERR_INVALID;
    }
    return send_owned(producer, MD_OP_PRODUCER_FRAME, payload, writer.size, fds, 2);
}

int md_producer_retire_done(md_producer_t* producer, uint64_t generation) {
    if (producer == NULL || !producer->pool_offered || !producer->retire_pending ||
        generation != producer->pool_generation) {
        return MD_ERR_STATE;
    }
    uint8_t payload[8];
    md_writer_t writer; md_writer_init(&writer, payload, sizeof(payload));
    if (md_proto_encode_u64(&writer, generation) != 0) return MD_ERR_INVALID;
    int rc = send_owned(producer, MD_OP_RETIRE_DONE, payload, writer.size, NULL, 0);
    if (rc == MD_OK) {
        producer->pool_offered = false;
        producer->retire_pending = false;
        producer->pool_generation = 0;
        producer->pool_buffer_count = 0;
    }
    return rc;
}
