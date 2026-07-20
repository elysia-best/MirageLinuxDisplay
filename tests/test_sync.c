#define _GNU_SOURCE

#include "mirage_display.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static void assert_closed(int fd) {
    errno = 0;
    assert(fcntl(fd, F_GETFD) == -1);
    assert(errno == EBADF);
}

int main(void) {
    int pipe_fds[2];
    assert(pipe2(pipe_fds, O_CLOEXEC) == 0);
    int release_fd = pipe_fds[0];
    close(pipe_fds[1]);
    assert(md_display_signal_release_syncobj(release_fd) == MD_ERR_IO);
    assert_closed(release_fd);

    int release_pipe[2];
    int sync_pipe[2];
    assert(pipe2(release_pipe, O_CLOEXEC) == 0);
    assert(pipe2(sync_pipe, O_CLOEXEC) == 0);
    release_fd = release_pipe[0];
    int sync_fd = sync_pipe[0];
    close(release_pipe[1]);
    close(sync_pipe[1]);
    assert(md_display_release_after_sync_file(release_fd, sync_fd) == MD_ERR_IO);
    assert_closed(release_fd);
    assert_closed(sync_fd);
    return 0;
}
