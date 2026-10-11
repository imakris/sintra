/* Isolated SDK capability evidence, never Sintra permission or death proof.
 * Registered Mach ports are fixture setup only, not a production proposal.
 * Raw syscall fork is diagnostic and makes no vendor support promise.
 */
#include <sys/appleapiopts.h>
/* Only the separately compiled diagnostic variant imports the SDK's syscall
 * selectors. The SDK marks syscall() unsupported; this trigger is not native
 * authority and supplies no public-API or vendor-compatibility conclusion.
 * The public variant never includes this private selector header.
 */
#ifdef N3_DIAGNOSTIC_RAW_FORK
#include <sys/syscall.h>
#endif
/* Default SDK feature macros may expose private declarations. Do not enable
 * those declarations merely to obtain a successful public decoder check.
 */
#undef __APPLE_API_PRIVATE
#include <bsm/libbsm.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_traps.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE_API_PRIVATE
#error "Private API declarations must remain disabled for this capability check"
#endif
#if defined(N3_DIAGNOSTIC_RAW_FORK) && !defined(SYS_fork)
#error "Installed SDK raw-fork diagnostic selector is unavailable"
#endif

enum { arm_message = 1, ready_message, confirm_message, release_message };

struct identity_t
{
    pid_t pid;
    int version;
};

struct message_t
{
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t destination;
    uint32_t stage;
    uint32_t nonce[4];
    pid_t libc_pid;
    struct identity_t self;
};

union receive_t
{
    struct message_t message;
    unsigned char bytes[sizeof(struct message_t) + sizeof(mach_msg_max_trailer_t) + 16];
    mach_msg_max_trailer_t alignment;
};

static void observation(const char *stage, long value, int error)
{
    printf("{\"stage\":\"%s\",\"return\":%ld,\"error\":%d}\n", stage, value, error);
    fflush(stdout);
}

/* Checked explicit cleanup calls retain their immediate native outcomes.
 * Mach errors are returned codes; POSIX errors are immediate errno values.
 * mach_msg_destroy is a void SDK API and supplies no native return to classify.
 */
static unsigned cleanup_failures;

static int native_close(const char *operation, int descriptor)
{
    int returned = close(descriptor);
    int error = returned != 0 ? errno : 0;
    observation(operation, returned, error);
    if (returned != 0) {
        ++cleanup_failures;
    }
    return returned;
}

static int native_mach_cleanup(const char *operation, kern_return_t returned)
{
    observation(operation, returned, returned != KERN_SUCCESS ? returned : 0);
    if (returned != KERN_SUCCESS) {
        ++cleanup_failures;
    }
    return returned == KERN_SUCCESS ? 0 : -1;
}

static void diagnostic_port(const char *stage, mach_port_t task, mach_port_t port, int slot)
{
    mach_port_type_t type = 0;
    kern_return_t returned = mach_port_type(task, port, &type);
    printf("{\"stage\":\"%s\",\"task_name\":%u,\"port_name\":%u,\"slot\":%d,"
           "\"return\":%d,\"type\":%u}\n", stage, (unsigned)task, (unsigned)port,
           slot, returned, (unsigned)type);
    fflush(stdout);
}

static int same_identity(struct identity_t a, struct identity_t b)
{
    return a.pid > 0 && a.pid == b.pid && a.version == b.version;
}

static struct identity_t project(audit_token_t token)
{
    struct identity_t identity = {audit_token_to_pid(token), audit_token_to_pidversion(token)};
    return identity;
}

/* A new non-exported pair queries the caller that is still executing here,
 * rather than an inherited connection's latest owner or libc PID cache.
 */
static int self_identity(struct identity_t *identity)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        observation("self_socketpair", -1, errno);
        return -1;
    }
    audit_token_t token;
    socklen_t size = sizeof(token);
    int returned = getsockopt(pair[0], SOL_LOCAL, LOCAL_PEERTOKEN, &token, &size);
    int error = returned != 0 ? errno : 0;
    observation("self_peer_token_query", returned, error);
    int close_a = native_close("cleanup_self_socket_0", pair[0]);
    int close_b = native_close("cleanup_self_socket_1", pair[1]);
    if (returned != 0 || size != sizeof(token) || close_a != 0 || close_b != 0) {
        return -1;
    }
    *identity = project(token);
    if (identity->pid <= 0) {
        observation("self_peer_token_invalid_pid", identity->pid, 0);
        return -1;
    }
    printf("{\"stage\":\"self_projected_identity\",\"pid\":%ld,\"version\":%d,"
           "\"libc_pid_diagnostic\":%ld}\n", (long)identity->pid, identity->version, (long)getpid());
    fflush(stdout);
    return 0;
}

static int new_receive(mach_port_t task, mach_port_t *port)
{
    kern_return_t returned = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, port);
    if (returned != KERN_SUCCESS) {
        observation("allocate_receive_port", returned, 0);
        return -1;
    }
    return 0;
}

static int send_message(mach_port_t destination, mach_port_t reply,
                        uint32_t stage, const uint32_t nonce[4],
                        mach_port_t transferred_receive)
{
    struct message_t message;
    memset(&message, 0, sizeof(message));
    if (self_identity(&message.self) != 0) {
        return -1;
    }
    message.libc_pid = getpid();
    message.stage = stage;
    if (nonce != NULL) {
        memcpy(message.nonce, nonce, sizeof(message.nonce));
    }
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
        reply != MACH_PORT_NULL ? MACH_MSG_TYPE_MAKE_SEND : 0);
    message.header.msgh_remote_port = destination;
    message.header.msgh_local_port = reply;
    message.header.msgh_size = sizeof(message);
    if (transferred_receive != MACH_PORT_NULL) {
        message.header.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
        message.body.msgh_descriptor_count = 1;
        message.destination.name = transferred_receive;
        message.destination.disposition = MACH_MSG_TYPE_MAKE_SEND;
        message.destination.type = MACH_MSG_PORT_DESCRIPTOR;
    }
    printf("{\"stage\":\"message_send_destination\",\"message_stage\":%u,"
           "\"destination\":%u,\"reply\":%u,\"sender_pid\":%ld}\n", stage,
           (unsigned)destination, (unsigned)reply, (long)message.self.pid);
    fflush(stdout);
    kern_return_t returned = mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
        sizeof(message), 0, MACH_PORT_NULL, 3000, MACH_PORT_NULL);
    observation("mach_send", returned, 0);
    return returned == MACH_MSG_SUCCESS ? 0 : -1;
}

static int receive_message(mach_port_t port, union receive_t *buffer,
                           struct identity_t *sender)
{
    observation("message_receive_port", port, 0);
    memset(buffer, 0, sizeof(*buffer));
    kern_return_t returned = mach_msg(&buffer->message.header,
        MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0)
        | MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT),
        0, sizeof(*buffer), port, 3000, MACH_PORT_NULL);
    observation("mach_receive", returned, 0);
    if (returned != MACH_MSG_SUCCESS) {
        return -1;
    }
    mach_msg_size_t size = buffer->message.header.msgh_size;
    size_t offset = (size + sizeof(natural_t) - 1) & ~(sizeof(natural_t) - 1);
    if (size != sizeof(struct message_t)
        || offset + sizeof(mach_msg_audit_trailer_t) > sizeof(*buffer))
    {
        observation("invalid_received_size", size, 0);
        mach_msg_destroy(&buffer->message.header);
        return -1;
    }
    mach_msg_audit_trailer_t *trailer = (mach_msg_audit_trailer_t *)(buffer->bytes + offset);
    if (trailer->msgh_trailer_type != MACH_MSG_TRAILER_FORMAT_0
        || trailer->msgh_trailer_size < sizeof(*trailer))
    {
        observation("invalid_audit_trailer", trailer->msgh_trailer_size, 0);
        mach_msg_destroy(&buffer->message.header);
        return -1;
    }
    *sender = project(trailer->msgh_audit);
    printf("{\"stage\":\"sender_identity\",\"message_stage\":%u,"
           "\"kernel_pid\":%ld,\"kernel_version\":%d,\"self_pid\":%ld,"
           "\"self_version\":%d,\"libc_pid_diagnostic\":%ld}\n",
           buffer->message.stage, (long)sender->pid, sender->version,
           (long)buffer->message.self.pid, buffer->message.self.version,
           (long)buffer->message.libc_pid);
    fflush(stdout);
    if (!same_identity(*sender, buffer->message.self)) {
        observation("sender_self_identity_mismatch", -1, 0);
        mach_msg_destroy(&buffer->message.header);
        return -1;
    }
    return 0;
}

static int child_messages(mach_port_t task, mach_port_t control,
                          struct identity_t parent)
{
    mach_port_t reply = MACH_PORT_NULL;
    mach_port_t data = MACH_PORT_NULL;
    union receive_t received;
    struct identity_t sender;
    int result = 21;
    if (new_receive(task, &reply) != 0
        || send_message(control, reply, arm_message, NULL, MACH_PORT_NULL) != 0
        || receive_message(reply, &received, &sender) != 0)
    {
        goto cleanup;
    }
    if (received.message.stage != ready_message || !same_identity(sender, parent)
        || received.message.header.msgh_remote_port != MACH_PORT_NULL
        || (received.message.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0
        || received.message.body.msgh_descriptor_count != 1
        || received.message.destination.type != MACH_MSG_PORT_DESCRIPTOR
        || received.message.destination.disposition != MACH_MSG_TYPE_PORT_SEND
        || !MACH_PORT_VALID(received.message.destination.name))
    {
        mach_msg_destroy(&received.message.header);
        goto cleanup;
    }
    data = received.message.destination.name;
    uint32_t nonce[4];
    memcpy(nonce, received.message.nonce, sizeof(nonce));
    received.message.destination.name = MACH_PORT_NULL;
    mach_msg_destroy(&received.message.header);
    observation("received_send_right_handoff", data, 0);
    if (send_message(data, reply, confirm_message, nonce, MACH_PORT_NULL) != 0
        || receive_message(reply, &received, &sender) != 0)
    {
        goto cleanup;
    }
    int released = received.message.stage == release_message
        && same_identity(sender, parent)
        && received.message.header.msgh_remote_port == MACH_PORT_NULL
        && (received.message.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0;
    mach_msg_destroy(&received.message.header);
    if (!released) {
        goto cleanup;
    }
    result = 0;
cleanup:
    if (data != MACH_PORT_NULL
        && native_mach_cleanup("cleanup_child_data_send",
                               mach_port_deallocate(task, data)) != 0)
    {
        result = 21;
    }
    if (reply != MACH_PORT_NULL
        && native_mach_cleanup("cleanup_child_reply_receive",
                               mach_port_destroy(task, reply)) != 0)
    {
        result = 21;
    }
    return cleanup_failures == 0 ? result : 21;
}

/* Only the parent identified by a fresh native self query consumes its owned
 * positive child PID. A signal status is recorded separately from normal exit.
 */
static int consume_child(pid_t child, int kill_child)
{
    if (kill_child && kill(child, SIGKILL) != 0 && errno != ESRCH) {
        observation("owned_child_kill_failed", -1, errno);
    }
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) {
        observation("owned_child_reap_failed", waited, errno);
        return -1;
    }
    printf("{\"stage\":\"owned_child_consumed\",\"pid\":%ld,\"raw_status\":%d,"
           "\"normal_exit\":%s,\"exit_code\":%d,\"signal\":%d}\n",
           (long)child, status, WIFEXITED(status) ? "true" : "false",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    fflush(stdout);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Retain the native count/error and every returned event field separately
 * from the derived match. A rejected event is evidence, not a zero return.
 */
static int observe_child_exit(int queue, pid_t child, const struct timespec *deadline)
{
    struct kevent event;
    int events = kevent(queue, NULL, 0, &event, 1, deadline);
    int error = events < 0 ? errno : 0;
    int observed = events == 1 && event.ident == (uintptr_t)child
        && event.filter == EVFILT_PROC && (event.fflags & NOTE_EXIT) != 0
        && (event.flags & EV_ERROR) == 0;
    printf("{\"stage\":\"exact_owned_child_exit_observed\",\"return\":%d,\"error\":%d,"
           "\"observed\":%s,\"event_present\":%s,\"expected_ident\":%ld,"
           "\"expected_filter\":%d,\"note_exit_flag\":%u,\"event_error_flag\":%u",
           events, error, observed ? "true" : "false", events > 0 ? "true" : "false",
           (long)child, (int)EVFILT_PROC, (unsigned)NOTE_EXIT, (unsigned)EV_ERROR);
    if (events > 0) {
        printf(",\"ident\":%llu,\"filter\":%d,\"flags\":%u,\"fflags\":%u,\"data\":%lld",
               (unsigned long long)event.ident, (int)event.filter,
               (unsigned)event.flags, (unsigned)event.fflags, (long long)event.data);
    }
    printf("}\n");
    fflush(stdout);
    return observed;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "fork") != 0 && strcmp(argv[1], "raw_fork") != 0)) {
        return 2;
    }
    int raw = strcmp(argv[1], "raw_fork") == 0;
#ifndef N3_DIAGNOSTIC_RAW_FORK
    if (raw) {
        observation("raw_fork_trigger_not_built", -1, 0);
        return 2;
    }
#endif
    struct identity_t original;
    if (self_identity(&original) != 0) {
        return 20;
    }
    (void)getpid();
    mach_port_t task = task_self_trap();
    mach_port_t control = MACH_PORT_NULL;
    mach_port_t data = MACH_PORT_NULL;
    mach_port_t arm_reply = MACH_PORT_NULL;
    mach_port_t confirm_reply = MACH_PORT_NULL;
    mach_port_t saved_bootstrap = MACH_PORT_NULL;
    int bootstrap_changed = 0;
    mach_port_array_t saved_ports = NULL;
    mach_msg_type_number_t saved_count = 0;
    int registered_changed = 0;
    int queue = -1;
    pid_t child = -1;
    int result = 21;
    if (new_receive(task, &control) != 0 || new_receive(task, &data) != 0) {
        goto cleanup;
    }
    kern_return_t returned = mach_port_insert_right(task, control, control, MACH_MSG_TYPE_MAKE_SEND);
    if (returned != KERN_SUCCESS) {
        observation("insert_control_send_right_failed", returned, 0);
        goto cleanup;
    }
    if (!raw) {
        returned = task_get_bootstrap_port(task, &saved_bootstrap);
        observation("parent_saved_bootstrap_query", returned, 0);
        if (returned != KERN_SUCCESS || !MACH_PORT_VALID(saved_bootstrap)) {
            goto cleanup;
        }
        returned = task_set_bootstrap_port(task, control);
        observation("parent_fixture_bootstrap_set", returned, 0);
        if (returned != KERN_SUCCESS) {
            goto cleanup;
        }
        bootstrap_changed = 1;
    }
    returned = mach_ports_lookup(task, &saved_ports, &saved_count);
    observation("parent_saved_registered_lookup", returned, 0);
    observation("parent_saved_registered_count", saved_count, 0);
    if (returned != KERN_SUCCESS || saved_ports == NULL
        || saved_count > MACH_PORTS_SLOTS_USED) {
        goto cleanup;
    }
    mach_port_t registered[MACH_PORTS_SLOTS_USED] = {MACH_PORT_NULL};
    for (mach_msg_type_number_t i = 0; i < saved_count; ++i) {
        registered[i] = saved_ports[i];
    }
    if (MACH_PORT_VALID(registered[SERVICE_SLOT])) {
        observation("fixture_service_slot_occupied", registered[SERVICE_SLOT], 0);
        goto cleanup;
    }
    registered[SERVICE_SLOT] = control;
    returned = mach_ports_register(task, registered, MACH_PORTS_SLOTS_USED);
    if (returned != KERN_SUCCESS) {
        observation("registered_control_port_failed", returned, 0);
        goto cleanup;
    }
    registered_changed = 1;
    diagnostic_port("parent_control_right", task, control, -1);
    diagnostic_port("parent_task_reply_right", task, mig_get_reply_port(), -1);
    observation("parent_cached_task_name", mach_task_self(), 0);
    observation("parent_bootstrap_name", bootstrap_port, 0);
    errno = 0;
#ifdef N3_DIAGNOSTIC_RAW_FORK
    long fork_return = raw ? syscall(SYS_fork) : fork();
#else
    long fork_return = fork();
#endif
    if (fork_return < 0) {
        observation("fork_failed", fork_return, errno);
        goto cleanup;
    }
    struct identity_t current;
    if (self_identity(&current) != 0) {
        /* Unknown fork role cannot consume a guessed PID. The isolated driver
         * retains the process group and its raw terminal outcome.
         */
        _exit(20);
    }
    if (current.pid != original.pid) {
        /* The parent-only observer queue is created after fork-role proof. */
        mach_port_t current_task = task_self_trap();
        observation("child_fresh_task_name", current_task, 0);
        observation("child_cached_task_name", mach_task_self(), 0);
        observation("child_bootstrap_name", bootstrap_port, 0);
        diagnostic_port("child_cached_reply_right", current_task, mig_get_reply_port(), -1);
        if (raw) {
            /* Diagnostic-only raw process creation skipped libSystem atfork.
             * Use the SDK-exposed cache and MIG reply APIs, not private runtime
             * entry points. Reject an unexpected live cached name before reset.
             */
            mach_port_t stale_reply = mig_get_reply_port();
            mach_port_type_t stale_type = 0;
            kern_return_t stale_status = mach_port_type(current_task, stale_reply, &stale_type);
            observation("raw_cached_reply_pre_reset", stale_status, 0);
            if (stale_status != KERN_INVALID_NAME) {
                observation("raw_cached_reply_reset_not_safe_for_fixture", stale_status, 0);
                _exit(20);
            }
            mach_task_self_ = current_task;
            mig_dealloc_reply_port(stale_reply); /* SDK API is void. */
            printf("{\"stage\":\"raw_public_reply_reset_attempted\",\"api_void\":true}\n");
            fflush(stdout);
            mach_port_t fresh_reply = mig_get_reply_port();
            mach_port_type_t fresh_type = 0;
            kern_return_t fresh_status = mach_port_type(current_task, fresh_reply, &fresh_type);
            printf("{\"stage\":\"raw_fresh_reply_checked\",\"return\":%d,"
                   "\"type\":%u,\"port_name\":%u}\n", fresh_status,
                   (unsigned)fresh_type, (unsigned)fresh_reply);
            fflush(stdout);
            if (fresh_status != KERN_SUCCESS || (fresh_type & MACH_PORT_TYPE_RECEIVE) == 0) {
                _exit(20);
            }
        }
        mach_port_array_t ports = NULL;
        mach_msg_type_number_t count = 0;
        returned = mach_ports_lookup(current_task, &ports, &count);
        observation("child_registered_port_lookup", returned, 0);
        observation("child_registered_port_count", count, 0);
        if (returned == KERN_SUCCESS && ports != NULL) {
            for (mach_msg_type_number_t i = 0; i < count; ++i) {
                diagnostic_port("child_registered_right", current_task, ports[i], (int)i);
            }
        }
        mach_port_t child_bootstrap = MACH_PORT_NULL;
        int child_result = 20;
        int selected_slot = raw ? SERVICE_SLOT : NAME_SERVER_SLOT;
        if (!raw) {
            kern_return_t bootstrap_result = task_get_bootstrap_port(current_task, &child_bootstrap);
            observation("child_current_bootstrap_query", bootstrap_result, 0);
            if (bootstrap_result == KERN_SUCCESS && MACH_PORT_VALID(child_bootstrap)) {
                diagnostic_port("child_current_bootstrap_right", current_task, child_bootstrap, -1);
                child_result = child_messages(current_task, child_bootstrap, original);
            }
        }
        else if (returned == KERN_SUCCESS && ports != NULL
            && count > (mach_msg_type_number_t)selected_slot
            && MACH_PORT_VALID(ports[selected_slot]))
        {
            observation("child_selected_fixture_slot", selected_slot, 0);
            child_result = child_messages(current_task, ports[selected_slot], original);
        }
        if (child_bootstrap != MACH_PORT_NULL
            && native_mach_cleanup("cleanup_child_current_bootstrap_send",
                    mach_port_deallocate(current_task, child_bootstrap)) != 0)
        {
            child_result = 21;
        }
        if (returned == KERN_SUCCESS && ports != NULL) {
            for (mach_msg_type_number_t i = 0; i < count; ++i) {
                if (MACH_PORT_VALID(ports[i])
                    && native_mach_cleanup("cleanup_child_registered_send",
                        mach_port_deallocate(current_task, ports[i])) != 0)
                {
                    child_result = 21;
                }
            }
            if (native_mach_cleanup("cleanup_child_lookup_vm",
                    vm_deallocate(current_task, (vm_address_t)ports,
                                  count * sizeof(*ports))) != 0)
            {
                child_result = 21;
            }
        }
        if (native_mach_cleanup("cleanup_child_task_send",
                mach_port_deallocate(current_task, current_task)) != 0)
        {
            child_result = 21;
        }
        _exit(cleanup_failures == 0 ? child_result : 21);
    }
    if (fork_return <= 0) {
        observation("parent_missing_owned_child_pid", fork_return, 0);
        goto cleanup;
    }
    child = (pid_t)fork_return;
    queue = kqueue();
    if (queue < 0) {
        observation("kqueue_failed", -1, errno);
        goto cleanup;
    }
    union receive_t received;
    struct identity_t armed;
    struct identity_t confirmed;
    diagnostic_port("parent_waiting_control_right", task, control, -1);
    if (receive_message(control, &received, &armed) != 0) {
        goto cleanup;
    }
    arm_reply = received.message.header.msgh_remote_port;
    received.message.header.msgh_remote_port = MACH_PORT_NULL;
    int valid_arm = received.message.stage == arm_message && armed.pid == child
        && MACH_PORT_VALID(arm_reply)
        && (received.message.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0;
    mach_msg_destroy(&received.message.header);
    if (!valid_arm) {
        goto cleanup;
    }
    struct kevent change;
    EV_SET(&change, child, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, NULL);
    int installed = kevent(queue, &change, 1, NULL, 0, NULL);
    observation("native_exit_filter_armed", installed, installed < 0 ? errno : 0);
    uint32_t nonce[4];
    arc4random_buf(nonce, sizeof(nonce));
    if (installed != 0 || send_message(arm_reply, MACH_PORT_NULL, ready_message, nonce, data) != 0
        || receive_message(data, &received, &confirmed) != 0)
    {
        goto cleanup;
    }
    confirm_reply = received.message.header.msgh_remote_port;
    received.message.header.msgh_remote_port = MACH_PORT_NULL;
    int valid_confirm = received.message.stage == confirm_message
        && MACH_PORT_VALID(confirm_reply)
        && (received.message.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0
        && same_identity(armed, confirmed)
        && memcmp(nonce, received.message.nonce, sizeof(nonce)) == 0;
    mach_msg_destroy(&received.message.header);
    if (!valid_confirm
        || send_message(confirm_reply, MACH_PORT_NULL, release_message, NULL, MACH_PORT_NULL) != 0)
    {
        goto cleanup;
    }
    struct timespec deadline = {3, 0};
    int observed = observe_child_exit(queue, child, &deadline);
    int consumed = consume_child(child, !observed);
    child = -1;
    if (observed && consumed == 0) {
        printf("{\"stage\":\"capability_observed\",\"case\":\"%s\","
               "\"standard_header_decoder_executed\":true,\"fresh_sender_matched_self\":true,"
               "\"post_arm_same_incarnation\":true,\"send_right_handoff\":true,"
               "\"native_exit_observed\":true,\"owned_child_consumed\":true,"
               "\"n3_acceptance\":false,\"pid_reuse_exercised\":false}\n", argv[1]);
        fflush(stdout);
        result = 0;
    }
cleanup:
    if (child > 0) {
        consume_child(child, 1);
    }
    if (queue >= 0 && native_close("cleanup_parent_queue", queue) != 0) {
        result = 21;
    }
    if (arm_reply != MACH_PORT_NULL
        && native_mach_cleanup("cleanup_parent_arm_send", mach_port_deallocate(task, arm_reply)) != 0)
    {
        result = 21;
    }
    if (confirm_reply != MACH_PORT_NULL
        && native_mach_cleanup("cleanup_parent_confirm_send", mach_port_deallocate(task, confirm_reply)) != 0)
    {
        result = 21;
    }
    if (bootstrap_changed
        && native_mach_cleanup("cleanup_parent_restore_bootstrap",
                task_set_bootstrap_port(task, saved_bootstrap)) != 0)
    {
        result = 21;
    }
    if (saved_bootstrap != MACH_PORT_NULL
        && native_mach_cleanup("cleanup_parent_saved_bootstrap_send",
                mach_port_deallocate(task, saved_bootstrap)) != 0)
    {
        result = 21;
    }
    if (registered_changed
        && native_mach_cleanup("cleanup_parent_restore_registered_ports",
            mach_ports_register(task, saved_ports, saved_count)) != 0)
    {
        result = 21;
    }
    if (saved_ports != NULL) {
        for (mach_msg_type_number_t i = 0; i < saved_count; ++i) {
            if (MACH_PORT_VALID(saved_ports[i])
                && native_mach_cleanup("cleanup_parent_saved_registered_send",
                    mach_port_deallocate(task, saved_ports[i])) != 0)
            {
                result = 21;
            }
        }
        if (native_mach_cleanup("cleanup_parent_saved_lookup_vm",
                vm_deallocate(task, (vm_address_t)saved_ports,
                              saved_count * sizeof(*saved_ports))) != 0)
        {
            result = 21;
        }
    }
    if (control != MACH_PORT_NULL
        && native_mach_cleanup("cleanup_parent_control_receive", mach_port_destroy(task, control)) != 0)
    {
        result = 21;
    }
    if (data != MACH_PORT_NULL
        && native_mach_cleanup("cleanup_parent_data_receive", mach_port_destroy(task, data)) != 0)
    {
        result = 21;
    }
    if (native_mach_cleanup("cleanup_parent_task_send",
            mach_port_deallocate(task, task)) != 0)
    {
        result = 21;
    }
    if (cleanup_failures != 0) {
        result = 21;
    }
    printf("{\"stage\":\"checked_cleanup_summary\",\"failures\":%u}\n", cleanup_failures);
    fflush(stdout);
    observation("fixture_cleanup_complete", result == 0, 0);
    return result;
}
