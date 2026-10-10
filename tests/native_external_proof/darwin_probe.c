/* Standalone Darwin native observations; no Sintra production code.
 * Guard prototypes/constants are from XNU xnu-11215.81.4 PRIVATE interfaces.
 * Successful execution does not establish vendor compatibility or N3 support.
 */
#define _DARWIN_C_SOURCE 1
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/event.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <mach/mach.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef int (*change_guard_fn)(int, const uint64_t *, unsigned,
                              const uint64_t *, unsigned, int *);
typedef int (*guard_close_fn)(int, const uint64_t *);
typedef int (*make_fileport_fn)(int, mach_port_t *);
static change_guard_fn change_guard;
static guard_close_fn guard_close;
static make_fileport_fn make_fileport;
enum { confined_set = 95, confined_get = 96, all_private_guards = 15 };

struct resource {
    int client, listener, accepted, guarded;
    uint64_t guard;
    char path[104];
};

static double monotonic_seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void observation(const char *stage, long value, int error) {
    printf("{\"stage\":\"%s\",\"actual_pid\":%ld,\"return\":%ld,\"errno\":%d}\n",
           stage, (long)getpid(), value, error);
    fflush(stdout);
}

static int join_child(pid_t child, int *status) {
    double now = monotonic_seconds();
    const double deadline = now + 5;
    struct timespec pause = {0, 10000000};
    int clock_failed = now < 0;
    for (unsigned attempts = 0; attempts < 500 && !clock_failed; ++attempts) {
        pid_t done = waitpid(child, status, WNOHANG);
        if (done == child) return 0;
        if (done < 0 && errno != EINTR) return -1;
        now = monotonic_seconds();
        if (now < 0) { clock_failed = 1; break; }
        if (now >= deadline) break;
        nanosleep(&pause, NULL);
    }
    if (clock_failed) observation("monotonic_clock_failed", -1, errno);
    /* Still our unreaped exact child; no PID-name lookup or unrelated kill. */
    int killed = kill(child, SIGKILL);
    observation("owned_child_deadline_kill", killed, killed < 0 ? errno : 0);
    /* Finite attempts also bound cleanup when the diagnostic clock fails. */
    for (unsigned attempts = 0; attempts < 200; ++attempts) {
        pid_t done = waitpid(child, status, WNOHANG);
        if (done == child) return 1;
        if (done < 0 && errno != EINTR) return -1;
        nanosleep(&pause, NULL);
    }
    return -1;
}

static int close_resource(struct resource *r) {
    int result = 0;
    if (r->client >= 0) {
        errno = 0;
        int closed = r->guarded ? guard_close(r->client, &r->guard) : close(r->client);
        observation("matching_client_close", closed, closed < 0 ? errno : 0);
        if (closed < 0) result = -1;
        r->client = -1;
    }
    if (r->accepted >= 0) close(r->accepted);
    if (r->listener >= 0) close(r->listener);
    if (r->path[0]) unlink(r->path);
    return result;
}

static int prepare_resource(struct resource *r, const char *directory) {
    memset(r, 0, sizeof(*r));
    r->client = r->listener = r->accepted = -1;
    if (snprintf(r->path, sizeof(r->path), "%s/control.sock", directory)
            >= (int)sizeof(r->path)) {
        observation("socket_path_too_long", -1, ENAMETOOLONG);
        return -1;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, r->path);
    address.sun_len = (uint8_t)(offsetof(struct sockaddr_un, sun_path)
                               + strlen(address.sun_path) + 1);
    r->listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (r->listener < 0 || bind(r->listener, (struct sockaddr *)&address,
                               address.sun_len) < 0 || listen(r->listener, 1) < 0) {
        observation("private_listener_setup", -1, errno);
        return -1;
    }
    r->client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (r->client < 0) { observation("client_socket", -1, errno); return -1; }
    arc4random_buf(&r->guard, sizeof(r->guard));
    r->guard |= 1;
    int fd_flags = 0;
    errno = 0;
    int changed = change_guard(r->client, NULL, 0, &r->guard,
                               all_private_guards, &fd_flags);
    observation("install_private_guards", changed, changed < 0 ? errno : 0);
    if (changed < 0) return -1;
    r->guarded = 1;
    errno = 0;
    int confined = fcntl(r->client, confined_set, 1);
    observation("set_confined", confined, confined < 0 ? errno : 0);
    if (confined < 0) return -1;
    errno = 0;
    int checked = fcntl(r->client, confined_get);
    observation("get_confined", checked, checked < 0 ? errno : 0);
    if (checked != 1) return -1;
    errno = 0;
    int connected = connect(r->client, (struct sockaddr *)&address, address.sun_len);
    observation("fresh_connect", connected, connected < 0 ? errno : 0);
    if (connected < 0) return -1;
    r->accepted = accept(r->listener, NULL, NULL);
    if (r->accepted < 0) { observation("accept", -1, errno); return -1; }
    pid_t peer = -1;
    socklen_t peer_size = sizeof(peer);
    errno = 0;
    int looked = getsockopt(r->accepted, SOL_LOCAL, LOCAL_PEERPID, &peer, &peer_size);
    printf("{\"stage\":\"latest_peer_pid_accessor\",\"return\":%d,\"errno\":%d,"
           "\"peer_pid\":%ld,\"actual_pid\":%ld,\"occurrence_proof\":false}\n",
           looked, looked < 0 ? errno : 0, (long)peer, (long)getpid());
    fflush(stdout);
    return 0;
}

static int observe_native_exit(void) {
    int release[2];
    if (pipe(release) < 0) { observation("release_pipe", -1, errno); return 21; }
    pid_t child = fork();
    if (child < 0) { observation("fork_exit_subject", -1, errno); return 21; }
    if (!child) {
        close(release[1]);
        alarm(8); /* diagnostic fixture bound, never native death evidence */
        char token;
        ssize_t got = read(release[0], &token, 1);
        _exit(got == 1 ? 0 : 31);
    }
    close(release[0]);
    int queue = kqueue();
    if (queue < 0) {
        observation("kqueue", -1, errno);
        close(release[1]); int status; join_child(child, &status); return 20;
    }
    struct kevent change, event;
    EV_SET(&change, (uintptr_t)child, EVFILT_PROC, EV_ADD | EV_ENABLE,
           NOTE_EXIT, 0, (void *)(uintptr_t)child);
    errno = 0;
    int installed = kevent(queue, &change, 1, NULL, 0, NULL);
    observation("install_note_exit", installed, installed < 0 ? errno : 0);
    if (installed < 0) {
        close(release[1]); int status; join_child(child, &status); close(queue); return 20;
    }
    ssize_t released = write(release[1], "X", 1);
    close(release[1]);
    struct timespec deadline = {5, 0};
    errno = 0;
    int count = kevent(queue, NULL, 0, &event, 1, &deadline);
    int event_error = count < 0 ? errno : 0;
    int matched = count == 1 && event.filter == EVFILT_PROC
                  && event.ident == (uintptr_t)child && (event.fflags & NOTE_EXIT)
                  && !(event.flags & EV_ERROR);
    printf("{\"stage\":\"native_exit_event\",\"return\":%d,\"errno\":%d,"
           "\"original_pid\":%ld,\"matched_original\":%s,"
           "\"released\":%ld,\"timeout_is_death\":false}\n",
           count, event_error, (long)child, matched ? "true" : "false", (long)released);
    fflush(stdout);
    int status = 0;
    int joined = join_child(child, &status);
    printf("{\"stage\":\"original_consuming_wait\",\"join_return\":%d,"
           "\"original_pid\":%ld,\"raw_status\":%d}\n",
           joined, (long)child, status);
    fflush(stdout);
    close(queue); /* native filter retained through event capture and child join */
    return matched && released == 1 && joined == 0
           && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 22;
}

int main(int argc, char **argv) {
    struct rlimit no_core = {0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
    if (argc != 3) return 64;
    const char *which = argv[1];
    if (strcmp(which, "native_exit") == 0) return observe_native_exit();
    change_guard = (change_guard_fn)dlsym(RTLD_DEFAULT, "change_fdguard_np");
    guard_close = (guard_close_fn)dlsym(RTLD_DEFAULT, "guarded_close_np");
    make_fileport = (make_fileport_fn)dlsym(RTLD_DEFAULT, "fileport_makeport");
    printf("{\"stage\":\"runtime_symbols\",\"change_fdguard_np\":%s,"
           "\"guarded_close_np\":%s,\"fileport_makeport\":%s,"
           "\"private_support_promise\":false}\n",
           change_guard ? "true" : "false", guard_close ? "true" : "false",
           make_fileport ? "true" : "false");
    fflush(stdout);
    if (!change_guard || !guard_close) return 20;
    struct resource r;
    if (prepare_resource(&r, argv[2]) != 0) { close_resource(&r); return 20; }
    int result = 0;
    if (strcmp(which, "baseline") == 0) {
        observation("baseline_private_setup_available", 0, 0);
    } else if (strcmp(which, "dup") == 0) {
        observation("attempt_guarded_dup", r.client, 0);
        errno = 0;
        int duplicate = dup(r.client);
        int error = duplicate < 0 ? errno : 0;
        observation("guarded_dup_returned", duplicate, error);
        if (duplicate >= 0) { close(duplicate); result = 23; }
    } else if (strcmp(which, "send_rights") == 0) {
        int transport[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, transport) < 0) {
            close_resource(&r); return 21;
        }
        char payload = 'X';
        struct iovec buffer = {&payload, 1};
        union {
            struct cmsghdr align;
            unsigned char bytes[CMSG_SPACE(sizeof(int))];
        } control;
        memset(&control, 0, sizeof(control));
        struct msghdr message;
        memset(&message, 0, sizeof(message));
        message.msg_iov = &buffer; message.msg_iovlen = 1;
        message.msg_control = control.bytes; message.msg_controllen = sizeof(control.bytes);
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &r.client, sizeof(int));
        observation("attempt_guarded_scm_rights", r.client, 0);
        errno = 0;
        ssize_t sent = sendmsg(transport[0], &message, 0);
        observation("guarded_scm_rights_returned", sent, sent < 0 ? errno : 0);
        close(transport[0]); close(transport[1]);
        if (sent >= 0) result = 23;
    } else if (strcmp(which, "fileport") == 0) {
        if (!make_fileport) { close_resource(&r); return 20; }
        mach_port_t port = MACH_PORT_NULL;
        observation("attempt_guarded_fileport", r.client, 0);
        errno = 0;
        int converted = make_fileport(r.client, &port);
        observation("guarded_fileport_returned", converted, converted < 0 ? errno : 0);
        if (converted == 0) {
            if (port != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), port);
            result = 23;
        }
    } else if (strcmp(which, "fork") == 0 || strcmp(which, "raw_fork") == 0) {
        pid_t original = getpid();
        errno = 0;
        long returned;
        if (strcmp(which, "raw_fork") == 0) {
#ifdef SYS_fork
            returned = syscall(SYS_fork);
#else
            observation("raw_fork_header_unavailable", -1, ENOSYS);
            close_resource(&r); return 20;
#endif
        } else {
            returned = fork();
        }
        if (returned < 0) { observation("fork_return", returned, errno); result = 20; }
        else if (getpid() != original) {
            /* Check native identity rather than Darwin raw-fork secondary return ABI. */
            errno = 0;
            int descriptor = fcntl(r.client, F_GETFD);
            int error = descriptor < 0 ? errno : 0;
            printf("{\"stage\":\"fork_child_descriptor\",\"original_pid\":%ld,"
                   "\"actual_pid\":%ld,\"return\":%d,\"errno\":%d}\n",
                   (long)original, (long)getpid(), descriptor, error);
            fflush(stdout);
            _exit(descriptor < 0 && error == EBADF ? 0 : 23);
        } else {
            int status = 0;
            int joined = join_child((pid_t)returned, &status);
            printf("{\"stage\":\"fork_child_consumed\",\"join_return\":%d,"
                   "\"child_pid\":%ld,\"raw_status\":%d}\n",
                   joined, returned, status);
            fflush(stdout);
            if (joined != 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) result = 22;
        }
    } else result = 64;
    if (close_resource(&r) != 0) result = 22;
    return result;
}
