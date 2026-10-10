/* Standalone native observations; no Sintra implementation or PID-reuse claim. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifdef __FreeBSD__
#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/event.h>
#include <sys/param.h>
#include <sys/procctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#endif

static void json_string(const char* in_text)
{
    putchar('"');
    for (const unsigned char* byte = (const unsigned char*)in_text; *byte; ++byte) {
        if (*byte == '"' || *byte == '\\') {
            putchar('\\');
            putchar(*byte);
        }
        else if (*byte < 32) {
            printf("\\u%04x", *byte);
        }
        else {
            putchar(*byte);
        }
    }
    putchar('"');
}

static void call_record(const char* in_case, const char* in_call, long in_return, int in_error)
{
    printf("{\"case\":\"%s\",\"call\":\"%s\",\"own_pid\":%ld,\"return\":%ld,\"errno\":%d}\n",
        in_case, in_call, (long)getpid(), in_return, in_error);
}

#if defined(__FreeBSD__) && defined(SOCK_CLOFORK) && defined(LOCAL_CREDS_PERSISTENT) \
    && defined(SCM_CREDS2) && defined(PROC_REAP_ACQUIRE) && defined(PROC_REAP_KILL)

enum { STEP_MILLISECONDS = 5000 };

struct child_report {
    int stage;
    int error;
    pid_t pid;
    long value;
    int flags;
};

static int monotonic_milliseconds(int64_t* out_milliseconds)
{
    struct timespec sample;
    if (clock_gettime(CLOCK_MONOTONIC, &sample) != 0) {
        return -1;
    }
    *out_milliseconds = (int64_t)sample.tv_sec * 1000 + sample.tv_nsec / 1000000;
    return 0;
}

static int wait_ready(int in_fd, short in_events)
{
    int64_t start;
    if (monotonic_milliseconds(&start) != 0) {
        return -1;
    }
    for (;;) {
        int64_t now;
        if (monotonic_milliseconds(&now) != 0) {
            return -1;
        }
        int64_t remaining = STEP_MILLISECONDS - (now - start);
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd descriptor = { .fd = in_fd, .events = in_events };
        int polled = poll(&descriptor, 1, (int)remaining);
        if (polled > 0) {
            if (descriptor.revents & (in_events | POLLHUP)) {
                return 0;
            }
            errno = EIO;
            return -1;
        }
        if (polled < 0 && errno == EINTR) {
            continue;
        }
        if (polled == 0) {
            errno = ETIMEDOUT;
        }
        return -1;
    }
}

static int pipe_read(int in_fd, size_t in_size, void* out_data)
{
    if (wait_ready(in_fd, POLLIN) != 0) {
        return -1;
    }
    ssize_t count = read(in_fd, out_data, in_size);
    if (count != (ssize_t)in_size) {
        if (count >= 0) {
            errno = EIO;
        }
        return -1;
    }
    return 0;
}

static int pipe_write(int in_fd, const void* in_data, size_t in_size)
{
    if (wait_ready(in_fd, POLLOUT) != 0) {
        return -1;
    }
    ssize_t count = write(in_fd, in_data, in_size);
    if (count != (ssize_t)in_size) {
        if (count >= 0) {
            errno = EIO;
        }
        return -1;
    }
    return 0;
}

static int child_record(int in_fd, int in_stage, long in_value, int in_error, int in_flags)
{
    struct child_report report = {0};
    report.stage = in_stage;
    report.error = in_error;
    report.pid = getpid();
    report.value = in_value;
    report.flags = in_flags;
    return pipe_write(in_fd, &report, sizeof(report));
}

static int reap_pid(pid_t in_pid, int in_expected_exit)
{
    int64_t start;
    if (monotonic_milliseconds(&start) != 0) {
        return -1;
    }
    for (;;) {
        int status = 0;
        pid_t waited = waitpid(in_pid, &status, WNOHANG);
        if (waited != 0) {
            call_record("custody", "waitpid", waited, waited < 0 ? errno : 0);
            printf("{\"case\":\"custody\",\"pid\":%ld,\"raw_status\":%d}\n", (long)in_pid, status);
            if (waited != in_pid || !WIFEXITED(status) || WEXITSTATUS(status) != in_expected_exit) {
                errno = ECHILD;
                return -1;
            }
            return 0;
        }
        int64_t now;
        if (monotonic_milliseconds(&now) != 0 || now - start >= STEP_MILLISECONDS) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct timespec pause = { .tv_nsec = 10000000 };
        nanosleep(&pause, NULL);
    }
}

/* Reaper ownership confines signals to this fixture's descendant tree. */
static int cleanup_children(void)
{
    int64_t start;
    if (monotonic_milliseconds(&start) != 0) {
        return -1;
    }
    int64_t last_kill = start - 500;
    for (;;) {
        int status;
        pid_t waited = waitpid(-1, &status, WNOHANG);
        if (waited > 0) {
            printf("{\"case\":\"cleanup\",\"reaped_pid\":%ld,\"raw_status\":%d}\n", (long)waited, status);
            continue;
        }
        if (waited < 0 && errno == ECHILD) {
            return 0;
        }
        if (waited < 0 && errno != EINTR) {
            return -1;
        }
        int64_t now;
        if (monotonic_milliseconds(&now) != 0 || now - start >= STEP_MILLISECONDS) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (now - last_kill >= 500) {
            struct procctl_reaper_kill request = {0};
            request.rk_sig = SIGKILL;
            int killed = procctl(P_PID, getpid(), PROC_REAP_KILL, &request);
            call_record("cleanup", "PROC_REAP_KILL_own_descendants", killed, killed < 0 ? errno : 0);
            printf("{\"case\":\"cleanup\",\"killed_count\":%u,\"failed_pid\":%ld}\n",
                request.rk_killed, (long)request.rk_fpid);
            if (killed != 0) {
                return -1;
            }
            last_kill = now;
        }
        struct timespec pause = { .tv_nsec = 10000000 };
        nanosleep(&pause, NULL);
    }
}

static int raw_fork_case(void)
{
    int protected_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOFORK | SOCK_CLOEXEC, 0);
    call_record("socket_capability", "socket_SOCK_CLOFORK", protected_fd, protected_fd < 0 ? errno : 0);
    if (protected_fd < 0) {
        return 2;
    }
    int ordinary_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    call_record("raw_fork", "socket_control", ordinary_fd, ordinary_fd < 0 ? errno : 0);
    int report_pipe[2] = {-1, -1};
    int result = 1;
    if (ordinary_fd < 0 || pipe(report_pipe) != 0) {
        call_record("raw_fork", "setup", -1, errno);
        goto finished;
    }
    int parent_flags = fcntl(protected_fd, F_GETFD);
    call_record("raw_fork", "parent_F_GETFD", parent_flags, parent_flags < 0 ? errno : 0);
    if (parent_flags < 0) {
        goto finished;
    }
    long child = syscall(SYS_fork);
    if (child == 0) {
        int protected_flags = fcntl(protected_fd, F_GETFD);
        int protected_error = protected_flags < 0 ? errno : 0;
        int ordinary_flags = fcntl(ordinary_fd, F_GETFD);
        int ordinary_error = ordinary_flags < 0 ? errno : 0;
        if (child_record(report_pipe[1], 1, protected_flags, protected_error, ordinary_flags) != 0
            || child_record(report_pipe[1], 2, ordinary_flags, ordinary_error, 0) != 0) {
            _exit(31);
        }
        _exit(0);
    }
    call_record("raw_fork", "SYS_fork", child, child < 0 ? errno : 0);
    if (child < 0) {
        result = 2;
        goto finished;
    }
    close(report_pipe[1]);
    report_pipe[1] = -1;
    struct child_report excluded, control;
    if (pipe_read(report_pipe[0], sizeof(excluded), &excluded) != 0
        || pipe_read(report_pipe[0], sizeof(control), &control) != 0) {
        call_record("raw_fork", "child_report", -1, errno);
        goto finished;
    }
    printf("{\"case\":\"raw_fork\",\"child_pid\":%ld,\"clofork_return\":%ld,\"clofork_errno\":%d,"
        "\"ordinary_return\":%ld,\"ordinary_errno\":%d}\n",
        (long)excluded.pid, excluded.value, excluded.error, control.value, control.error);
    if (excluded.pid == child && control.pid == child && excluded.stage == 1 && control.stage == 2
        && excluded.value == -1 && excluded.error == EBADF && control.value >= 0 && control.error == 0
        && reap_pid((pid_t)child, 0) == 0) {
        result = 0;
    }
finished:
    if (report_pipe[0] >= 0) { close(report_pipe[0]); }
    if (report_pipe[1] >= 0) { close(report_pipe[1]); }
    if (ordinary_fd >= 0) { close(ordinary_fd); }
    close(protected_fd);
    return result;
}

static int receive_credentials(int in_fd, char in_byte, pid_t in_expected_pid)
{
    union { struct cmsghdr alignment; unsigned char bytes[8192]; } control = {0};
    char byte = 0;
    struct iovec vector = { .iov_base = &byte, .iov_len = 1 };
    struct msghdr message = {0};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    if (wait_ready(in_fd, POLLIN) != 0) {
        return -1;
    }
    ssize_t received = recvmsg(in_fd, &message, MSG_DONTWAIT);
    call_record("rfork_credentials", "recvmsg", received, received < 0 ? errno : 0);
    if (received != 1 || byte != in_byte || (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC))) {
        errno = EPROTO;
        return -1;
    }
    int count = 0;
    for (struct cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_CREDS2) {
            continue;
        }
        size_t prefix = offsetof(struct sockcred2, sc_groups);
        if (header->cmsg_len < CMSG_LEN(prefix)) {
            errno = EPROTO;
            return -1;
        }
        struct sockcred2 credentials = {0};
        memcpy(&credentials, CMSG_DATA(header), prefix);
        size_t available = header->cmsg_len - CMSG_LEN(prefix);
        printf("{\"case\":\"rfork_credentials\",\"message\":\"%c\",\"native_pid\":%ld,"
            "\"expected_pid\":%ld,\"version\":%d,\"uid\":%lu,\"euid\":%lu,\"groups\":%d}\n",
            byte, (long)credentials.sc_pid, (long)in_expected_pid, credentials.sc_version,
            (unsigned long)credentials.sc_uid, (unsigned long)credentials.sc_euid, credentials.sc_ngroups);
        if (credentials.sc_version != 0 || credentials.sc_pid != in_expected_pid || credentials.sc_ngroups < 0
            || (size_t)credentials.sc_ngroups > available / sizeof(gid_t)) {
            errno = EPROTO;
            return -1;
        }
        ++count;
    }
    if (count != 1) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static void send_controlled_byte(int in_control_fd, int in_socket_fd, int in_report_fd, char in_byte, int in_stage)
{
    char command;
    if (pipe_read(in_control_fd, 1, &command) != 0 || command != in_byte) {
        _exit(31);
    }
    ssize_t sent = send(in_socket_fd, &in_byte, 1, MSG_DONTWAIT);
    int send_error = sent < 0 ? errno : 0;
    if (child_record(in_report_fd, in_stage, sent, send_error, 0) != 0 || sent != 1) {
        _exit(31);
    }
}

static int rfork_case(void)
{
    char directory[] = "/tmp/sintra-freebsd-probe-XXXXXX";
    struct sockaddr_un address = {0};
    int listener = -1, connection = -1, queue = -1;
    int pipes[6] = {-1, -1, -1, -1, -1, -1};
    int result = 1;
    int directory_created = mkdtemp(directory) != NULL;
    if (!directory_created) {
        call_record("rfork", "mkdtemp", -1, errno);
        return 1;
    }
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s/control.sock", directory);
    address.sun_len = (unsigned char)SUN_LEN(&address);
    listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener < 0 || bind(listener, (struct sockaddr*)&address, address.sun_len) != 0
        || listen(listener, 1) != 0 || pipe(&pipes[0]) != 0 || pipe(&pipes[2]) != 0 || pipe(&pipes[4]) != 0) {
        call_record("rfork", "listener_or_pipe_setup", -1, errno);
        goto finished;
    }
    pid_t original = fork();
    if (original == 0) {
        close(listener);
        close(pipes[0]); close(pipes[3]); close(pipes[5]);
        int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOFORK | SOCK_CLOEXEC, 0);
        int error = client < 0 ? errno : 0;
        if (child_record(pipes[1], 1, client, error, 0) != 0 || client < 0) { _exit(31); }
        int connected = connect(client, (struct sockaddr*)&address, address.sun_len);
        error = connected < 0 ? errno : 0;
        if (child_record(pipes[1], 2, connected, error, 0) != 0 || connected < 0) { _exit(31); }
        int flags = fcntl(client, F_GETFD);
        error = flags < 0 ? errno : 0;
        if (child_record(pipes[1], 3, flags, error, 0) != 0 || flags < 0) { _exit(31); }
        pid_t descendant = rfork(RFPROC); /* No RFFDG and no shared address space. */
        if (descendant == 0) {
            /* Closing any shared fd here would also change the original's table. */
            send_controlled_byte(pipes[4], client, pipes[1], 'C', 6);
            _exit(0);
        }
        error = descendant < 0 ? errno : 0;
        if (child_record(pipes[1], 4, descendant, error, RFPROC) != 0 || descendant < 0) { _exit(31); }
        send_controlled_byte(pipes[2], client, pipes[1], 'A', 5);
        char exit_command;
        if (pipe_read(pipes[2], 1, &exit_command) != 0 || exit_command != 'X') { _exit(31); }
        _exit(19);
    }
    call_record("rfork", "original_fork", original, original < 0 ? errno : 0);
    if (original < 0) { goto finished; }
    close(pipes[1]); pipes[1] = -1;
    close(pipes[2]); pipes[2] = -1;
    close(pipes[4]); pipes[4] = -1;
    struct child_report report = {0};
    const char* calls[] = {"socket_SOCK_CLOFORK", "connect", "F_GETFD", "rfork_RFPROC_without_RFFDG"};
    for (int stage = 1; stage <= 4; ++stage) {
        if (pipe_read(pipes[0], sizeof(report), &report) != 0) {
            call_record("rfork", "setup_child_report", -1, errno);
            goto finished;
        }
        printf("{\"case\":\"rfork\",\"call\":\"%s\",\"caller_pid\":%ld,\"return\":%ld,"
            "\"errno\":%d,\"rfork_flags\":%d}\n", calls[stage - 1], (long)report.pid,
            report.value, report.error, report.flags);
        if (report.stage != stage || report.pid != original || report.value < 0 || report.error != 0) {
            result = report.value < 0 ? 2 : 1;
            goto finished;
        }
    }
    pid_t descendant = (pid_t)report.value;
    if (wait_ready(listener, POLLIN) != 0) { goto finished; }
    connection = accept(listener, NULL, NULL);
    call_record("rfork", "accept", connection, connection < 0 ? errno : 0);
    if (connection < 0) { goto finished; }
    int enabled = 1;
    int configured = setsockopt(connection, SOL_LOCAL, LOCAL_CREDS_PERSISTENT, &enabled, sizeof(enabled));
    call_record("rfork_credentials", "LOCAL_CREDS_PERSISTENT", configured, configured < 0 ? errno : 0);
    if (configured != 0) { result = 2; goto finished; }
    queue = kqueue();
    call_record("native_exit", "kqueue", queue, queue < 0 ? errno : 0);
    if (queue < 0) { result = 2; goto finished; }
    struct kevent change, retained_exit = {0}, early_event;
    EV_SET(&change, original, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    int installed = kevent(queue, &change, 1, NULL, 0, NULL);
    call_record("native_exit", "install_NOTE_EXIT", installed, installed < 0 ? errno : 0);
    if (installed != 0) { result = 2; goto finished; }
    struct timespec no_wait = {0};
    int early = kevent(queue, NULL, 0, &early_event, 1, &no_wait);
    call_record("native_exit", "before_controlled_exit", early, early < 0 ? errno : 0);
    if (early != 0 || pipe_write(pipes[3], "A", 1) != 0
        || receive_credentials(connection, 'A', original) != 0
        || pipe_read(pipes[0], sizeof(report), &report) != 0
        || report.stage != 5 || report.pid != original || report.value != 1 || report.error != 0) {
        call_record("rfork", "ARM_observation", -1, errno);
        goto finished;
    }
    call_record("rfork", "original_send", report.value, report.error);
    if (pipe_write(pipes[3], "X", 1) != 0) { goto finished; }
    struct timespec exit_deadline = { .tv_sec = 5 };
    int exited = kevent(queue, NULL, 0, &retained_exit, 1, &exit_deadline);
    call_record("native_exit", "await_NOTE_EXIT", exited, exited < 0 ? errno : 0);
    printf("{\"case\":\"native_exit\",\"original_pid\":%ld,\"event_ident\":%lu,\"filter\":%d,"
        "\"flags\":%u,\"fflags\":%u,\"data\":%ld}\n", (long)original,
        (unsigned long)retained_exit.ident, retained_exit.filter, retained_exit.flags,
        retained_exit.fflags, (long)retained_exit.data);
    if (exited != 1 || retained_exit.ident != (uintptr_t)original || retained_exit.filter != EVFILT_PROC
        || !(retained_exit.fflags & NOTE_EXIT) || (retained_exit.flags & EV_ERROR)
        || reap_pid(original, 19) != 0) {
        goto finished;
    }
    /* Only native exit plus reaping precede this fresh descendant command. */
    if (pipe_write(pipes[5], "C", 1) != 0 || receive_credentials(connection, 'C', descendant) != 0
        || pipe_read(pipes[0], sizeof(report), &report) != 0
        || report.stage != 6 || report.pid != descendant || report.value != 1 || report.error != 0
        || descendant == original || reap_pid(descendant, 0) != 0) {
        call_record("rfork", "surviving_channel_observation", -1, errno);
        goto finished;
    }
    call_record("rfork", "descendant_send_after_original_exit", report.value, report.error);
    printf("{\"case\":\"rfork\",\"status\":\"observed\",\"original_pid\":%ld,\"descendant_pid\":%ld,"
        "\"original_exit_fact_retained\":true,\"kqueue_retained_through_message\":true,"
        "\"pid_reuse_exercised\":false,\"sintra_process_word_exercised\":false}\n",
        (long)original, (long)descendant);
    result = 0;
finished:
    if (result != 0) { call_record("rfork", "case_incomplete", -1, errno); }
    if (queue >= 0) { close(queue); }
    if (connection >= 0) { close(connection); }
    if (listener >= 0) { close(listener); }
    for (int i = 0; i < 6; ++i) { if (pipes[i] >= 0) { close(pipes[i]); } }
    unlink(address.sun_path);
    rmdir(directory);
    return result;
}
#endif

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    struct utsname runtime;
    int named = uname(&runtime);
    call_record("runtime", "uname", named, named < 0 ? errno : 0);
    if (named != 0) { return 1; }
    printf("{\"probe\":\"freebsd_native_fd_table\",\"runtime\":{\"system\":"); json_string(runtime.sysname);
    printf(",\"release\":"); json_string(runtime.release);
    printf(",\"version\":"); json_string(runtime.version);
    printf(",\"machine\":"); json_string(runtime.machine);
    printf("},\"pid_reuse_exercised\":false,\"sintra_process_word_exercised\":false}\n");
#if defined(__FreeBSD__) && defined(SOCK_CLOFORK) && defined(LOCAL_CREDS_PERSISTENT) \
    && defined(SCM_CREDS2) && defined(PROC_REAP_ACQUIRE) && defined(PROC_REAP_KILL)
    int kernel_release = 0;
    size_t release_size = sizeof(kernel_release);
    int queried = sysctlbyname("kern.osreldate", &kernel_release, &release_size, NULL, 0);
    call_record("runtime", "kern.osreldate", queried, queried < 0 ? errno : 0);
    printf("{\"case\":\"runtime\",\"header_FreeBSD_version\":%d,\"kernel_osreldate\":%d,"
        "\"kernel_osreldate_available\":%s}\n", __FreeBSD_version, kernel_release,
        queried == 0 && release_size == sizeof(kernel_release) ? "true" : "false");
    struct sigaction ignored = {0};
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    if (sigaction(SIGPIPE, &ignored, NULL) != 0) { return 1; }
    int acquired = procctl(P_PID, getpid(), PROC_REAP_ACQUIRE, NULL);
    call_record("custody", "PROC_REAP_ACQUIRE_self", acquired, acquired < 0 ? errno : 0);
    if (acquired != 0) {
        printf("{\"status\":\"unavailable\",\"reason\":\"fixture_child_custody\"}\n");
        return 2;
    }
    int copied = raw_fork_case();
    int shared = copied == 0 ? rfork_case() : -1;
    int cleanup = cleanup_children();
    call_record("cleanup", "settled", cleanup, cleanup < 0 ? errno : 0);
    int result = cleanup != 0 ? 1 : copied == 1 || shared == 1 ? 1 : copied == 2 || shared == 2 ? 2 : 0;
    printf("{\"status\":\"%s\",\"raw_fork_result\":%d,\"rfork_result\":%d,\"rfork_ran\":%s,"
        "\"cleanup_result\":%d,\"n3_acceptance\":false}\n",
        result == 0 ? "supported_observed" : result == 2 ? "unavailable" : "probe_error",
        copied, shared, shared < 0 ? "false" : "true", cleanup);
    return result;
#else
    printf("{\"status\":\"unavailable\",\"reason\":\"requires_native_FreeBSD_and_required_headers\","
        "\"n3_acceptance\":false}\n");
    return 2;
#endif
}
