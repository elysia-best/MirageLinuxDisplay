#define _GNU_SOURCE

#include "mirage_display.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef DRM_IOCTL_BASE
#define DRM_IOCTL_BASE 'd'
#endif

struct md_drm_syncobj_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
    uint32_t pad;
};
struct md_drm_syncobj_create {
    uint32_t handle;
    uint32_t flags;
};
struct md_drm_syncobj_destroy {
    uint32_t handle;
    uint32_t pad;
};
struct md_drm_syncobj_transfer {
    uint32_t src_handle;
    uint32_t dst_handle;
    uint64_t src_point;
    uint64_t dst_point;
    uint32_t flags;
    uint32_t pad;
};
struct md_drm_syncobj_array {
    uint64_t handles;
    uint32_t count_handles;
    uint32_t pad;
};

#define MD_DRM_IOCTL_SYNCOBJ_CREATE \
    _IOWR(DRM_IOCTL_BASE, 0xbf, struct md_drm_syncobj_create)
#define MD_DRM_IOCTL_SYNCOBJ_DESTROY \
    _IOWR(DRM_IOCTL_BASE, 0xc0, struct md_drm_syncobj_destroy)
#define MD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE \
    _IOWR(DRM_IOCTL_BASE, 0xc2, struct md_drm_syncobj_handle)
#define MD_DRM_IOCTL_SYNCOBJ_SIGNAL \
    _IOWR(DRM_IOCTL_BASE, 0xc5, struct md_drm_syncobj_array)
#define MD_DRM_IOCTL_SYNCOBJ_TRANSFER \
    _IOWR(DRM_IOCTL_BASE, 0xcc, struct md_drm_syncobj_transfer)

#define MD_DRM_SYNCOBJ_FD_TO_HANDLE_IMPORT_SYNC_FILE (UINT32_C(1) << 0)

static int open_render_node(void) {
    for (int minor = 128; minor <= 255; ++minor) {
        char path[64];
        int written = snprintf(path, sizeof(path), "/dev/dri/renderD%d", minor);
        if (written <= 0 || (size_t)written >= sizeof(path)) continue;
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
    return -1;
}

int md_display_signal_release_syncobj(int release_syncobj_fd) {
    if (release_syncobj_fd < 0) return MD_ERR_INVALID;
    int drm_fd = open_render_node();
    if (drm_fd < 0) {
        close(release_syncobj_fd);
        return MD_ERR_IO;
    }
    struct md_drm_syncobj_handle import = {
        .handle = 0,
        .flags = 0,
        .fd = release_syncobj_fd,
        .pad = 0,
    };
    int result = MD_ERR_IO;
    if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import) == 0) {
        uint32_t handles[1] = {import.handle};
        struct md_drm_syncobj_array signal = {
            .handles = (uint64_t)(uintptr_t)handles,
            .count_handles = 1,
            .pad = 0,
        };
        if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_SIGNAL, &signal) == 0) result = MD_OK;
        struct md_drm_syncobj_destroy destroy = {.handle = import.handle, .pad = 0};
        (void)ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
    close(release_syncobj_fd);
    close(drm_fd);
    return result;
}

int md_display_release_after_sync_file(int release_syncobj_fd, int sync_file_fd) {
    if (release_syncobj_fd < 0 || sync_file_fd < 0) {
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        if (sync_file_fd >= 0) close(sync_file_fd);
        return MD_ERR_INVALID;
    }
    int drm_fd = open_render_node();
    if (drm_fd < 0) {
        close(release_syncobj_fd);
        close(sync_file_fd);
        return MD_ERR_IO;
    }

    int result = MD_ERR_IO;
    struct md_drm_syncobj_handle release = {
        .handle = 0,
        .flags = 0,
        .fd = release_syncobj_fd,
        .pad = 0,
    };
    if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &release) != 0) goto done;

    struct md_drm_syncobj_create source = {.handle = 0, .flags = 0};
    if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_CREATE, &source) != 0) goto destroy_release;

    struct md_drm_syncobj_handle sync_file = {
        .handle = source.handle,
        .flags = MD_DRM_SYNCOBJ_FD_TO_HANDLE_IMPORT_SYNC_FILE,
        .fd = sync_file_fd,
        .pad = 0,
    };
    if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &sync_file) == 0) {
        struct md_drm_syncobj_transfer transfer = {
            .src_handle = source.handle,
            .dst_handle = release.handle,
            .src_point = 0,
            .dst_point = 0,
            .flags = 0,
            .pad = 0,
        };
        if (ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_TRANSFER, &transfer) == 0) result = MD_OK;
    }
    {
        struct md_drm_syncobj_destroy destroy = {.handle = source.handle, .pad = 0};
        (void)ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
destroy_release:
    {
        struct md_drm_syncobj_destroy destroy = {.handle = release.handle, .pad = 0};
        (void)ioctl(drm_fd, MD_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
done:
    close(release_syncobj_fd);
    close(sync_file_fd);
    close(drm_fd);
    return result;
}
