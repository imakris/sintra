#include <sintra/sintra.h>
#include <sintra/detail/ipc/process_utils.h>

#include "test_utils.h"

#include <array>
#include <new>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <fcntl.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/file.h>
#endif
#endif

#ifndef _WIN32
namespace {

using sintra::detail::Cleanup_domain;
using sintra::Process_metadata_state;

std::optional<Cleanup_domain> s_domain;
std::uint64_t s_first_id = 0;
std::uint64_t s_second_id = 0;
int s_draws = 0;
std::uint32_t s_dead_pid = 0;
#if defined(__linux__)
std::filesystem::path s_replacement_path;
bool s_replaced = false;
std::filesystem::path s_teardown_path;
bool s_teardown_replaced = false;
#endif

std::optional<Cleanup_domain> injected_domain() { return s_domain; }

std::uint64_t injected_id()
{
    return ++s_draws == 1 ? s_first_id : s_second_id;
}

void require(bool condition, const char* message)
{
    sintra::test::require_true(condition, "private_swarm_root_test: ", message);
}

class Temp_directory_env
{
public:
    explicit Temp_directory_env(const std::filesystem::path& directory)
    {
        if (const char* previous = std::getenv("TMPDIR")) {
            m_previous = previous;
        }
        require(::setenv("TMPDIR", directory.string().c_str(), 1) == 0,
            "set temporary directory");
    }

    ~Temp_directory_env()
    {
        if (m_previous) { ::setenv("TMPDIR", m_previous->c_str(), 1); }
        else { ::unsetenv("TMPDIR"); }
    }

private:
    std::optional<std::string> m_previous;
};

std::filesystem::path make_stale_directory(std::uint64_t id, const std::optional<Cleanup_domain>& domain)
{
    const auto directory = sintra::detail::private_swarm_root(id);
    require(sintra::detail::create_private_swarm_directory_exclusive(directory) ==
        sintra::detail::private_swarm_create_result::created,
        "create direct swarm fixture");
    if (domain) {
        require(sintra::detail::publish_private_file(
            directory / sintra::detail::private_cleanup_domain_filename(),
            sintra::detail::private_cleanup_domain_contents(*domain)),
            "publish cleanup domain");
    }
    sintra::run_marker_record_t marker{};
    marker.pid = s_dead_pid;
    require(sintra::write_run_marker(directory, marker), "publish stale marker");
    return directory;
}

void scan(const std::filesystem::path& temp_directory)
{
    sintra::cleanup_stale_private_swarms(temp_directory);
}

void test_coordinator_allocation(const std::filesystem::path& temp_directory, int argc, char* argv[])
{
    const auto former_root = temp_directory / ("sintra-" + std::to_string(::geteuid()));
    require(sintra::detail::write_private_file(former_root, "foreign obstruction"),
        "seed former per-account root obstruction");

    s_first_id = 0x16a39c082744912eull;
    s_second_id = 0x16a39c082744912full;
    const auto collision = sintra::detail::private_swarm_root(s_first_id);
    require(sintra::detail::create_private_swarm_directory_exclusive(collision) ==
        sintra::detail::private_swarm_create_result::created,
        "seed an owned ID collision");
    s_draws = 0;
    sintra::detail::draw_private_swarm_id_for_test = injected_id;
    sintra::init(argc, argv);
    const auto session = std::filesystem::path(sintra::s_mproc->m_directory);
    require(s_draws == 2 && sintra::s_mproc->m_swarm_id == s_second_id,
        "coordinator redraws after an owned collision");
    require(session == sintra::detail::private_swarm_root(s_second_id, true) &&
        sintra::detail::private_directory_owned(session),
        "coordinator publishes the direct private directory");
    require(std::filesystem::exists(collision) && std::filesystem::exists(former_root),
        "collisions and former root obstruction are untouched");
    sintra::detail::finalize();
    sintra::detail::draw_private_swarm_id_for_test = nullptr;
    require(!std::filesystem::exists(session), "normal finalize removes the direct root");
}

void test_domains(const std::filesystem::path& temp_directory)
{
    Cleanup_domain comparable{"3f855a31-247b-4c72-a83b-14b4a5f3b678",
        {Process_metadata_state::VALID, 4, 1101},
        {Process_metadata_state::VALID, 4, 2202}};
    s_domain = comparable;
    sintra::detail::private_cleanup_domain_for_test = injected_domain;
    const auto stale = make_stale_directory(0x22a1000000000001ull, comparable);
    auto other_time = comparable;
    other_time.time.inode += 1;
    const auto other_namespace = make_stale_directory(0x22a1000000000002ull, other_time);
    const auto unknown = make_stale_directory(0x22a1000000000003ull, std::nullopt);
    const auto malformed = make_stale_directory(0x22a1000000000004ull, comparable);
    require(sintra::detail::publish_private_file(
        malformed / sintra::detail::private_cleanup_domain_filename(),
        "sintra-cleanup-domain=1\nboot=partial\n"), "replace with partial sidecar");
    const auto permissive = make_stale_directory(0x22a1000000000006ull, comparable);
    require(::chmod(permissive.c_str(), 0755) == 0, "make candidate permissive");
    const auto linked = sintra::detail::private_swarm_root(0x22a1000000000007ull);
    std::filesystem::create_directory_symlink(other_namespace, linked);

    s_domain.reset();
    scan(temp_directory);
    require(std::filesystem::exists(stale), "unknown observer metadata disables scanning");
    s_domain = comparable;
    scan(temp_directory);
    require(!std::filesystem::exists(stale), "same-domain stale root is removed");
    require(std::filesystem::exists(other_namespace), "live cross-namespace root is preserved");
    require(std::filesystem::exists(unknown) && std::filesystem::exists(malformed),
        "missing and partial sidecars are preserved");
    require(std::filesystem::exists(permissive) && std::filesystem::is_symlink(linked),
        "permissive and linked candidates are preserved");
    require(::chmod(permissive.c_str(), 0700) == 0, "restore candidate permissions");

    auto absent = comparable;
    absent.time = {Process_metadata_state::ABSENT, 0, 0};
    s_domain = absent;
    const auto absent_stale = make_stale_directory(0x22a1000000000005ull, absent);
    scan(temp_directory);
    require(!std::filesystem::exists(absent_stale),
        "positively established absent feature forms a comparable domain");
    s_domain.reset();
    sintra::detail::private_cleanup_domain_for_test = nullptr;
}

void test_complete_scan(const std::filesystem::path& temp_directory)
{
    Cleanup_domain comparable{"dc82a410-5980-496f-81c9-c977677e1f2d",
        {Process_metadata_state::VALID, 5, 3101},
        {Process_metadata_state::VALID, 5, 3202}};
    s_domain = comparable;
    sintra::detail::private_cleanup_domain_for_test = injected_domain;
    for (std::uint64_t id = 0x33a1000000000001ull; id <= 0x33a1000000000046ull; ++id) {
        (void)make_stale_directory(id, comparable);
    }
    for (unsigned i = 0; i < 1100; ++i) {
        require(sintra::detail::write_private_file(temp_directory / ("unrelated-" + std::to_string(i)), ""),
            "seed unrelated temporary entries");
    }
    scan(temp_directory);
    std::size_t remaining = 0;
    for (std::uint64_t id = 0x33a1000000000001ull; id <= 0x33a1000000000046ull; ++id) {
        remaining += std::filesystem::exists(sintra::detail::private_swarm_root(id));
    }
    require(remaining == 0,
        "one scan reaches all stale candidates");
    s_domain.reset();
    sintra::detail::private_cleanup_domain_for_test = nullptr;
}

#if defined(__linux__)
void replace_stale_directory(const std::filesystem::path& path)
{
    if (path != s_replacement_path) {
        return;
    }
    const int contender = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(contender >= 0, "open a second handle to the candidate");
    errno = 0;
    const int lock_result = ::flock(contender, LOCK_EX | LOCK_NB);
    const bool locked_by_scanner = lock_result != 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
    if (lock_result == 0) {
        ::flock(contender, LOCK_UN);
    }
    ::close(contender);
    require(locked_by_scanner, "scanner holds the candidate lock through deletion");
    require(sintra::detail::remove_private_directory_tree(path),
        "remove the validated stale directory before deletion");
    require(sintra::detail::create_private_swarm_directory_exclusive(path) ==
        sintra::detail::private_swarm_create_result::created,
        "reuse the stale directory's exact name");
    sintra::detail::publish_private_cleanup_domain(path);
    sintra::run_marker_record_t marker{};
    marker.pid = sintra::get_current_pid();
    require(sintra::write_run_marker(path, marker), "publish replacement's live marker");
    s_replaced = true;
}

void test_name_reuse_during_cleanup(const std::filesystem::path& temp_directory)
{
    s_domain = Cleanup_domain{"fca857bc-4e1d-4d37-941c-909c0e4fda21",
        {Process_metadata_state::VALID, 7, 5101},
        {Process_metadata_state::VALID, 7, 5202}};
    sintra::detail::private_cleanup_domain_for_test = injected_domain;
    s_replacement_path = make_stale_directory(0x44a1000000000001ull, s_domain);
    const int blocker = ::open(s_replacement_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(blocker >= 0 && ::flock(blocker, LOCK_EX) == 0,
        "hold a competing cleanup lock");
    scan(temp_directory);
    require(std::filesystem::exists(s_replacement_path),
        "a busy cleanup lock preserves its candidate");
    require(::flock(blocker, LOCK_UN) == 0 && ::close(blocker) == 0,
        "release the competing cleanup lock");
    s_replaced = false;
    sintra::detail::before_private_swarm_removal_for_test = replace_stale_directory;
    scan(temp_directory);
    sintra::detail::before_private_swarm_removal_for_test = nullptr;
    const auto marker = sintra::read_run_marker(sintra::run_marker_path(s_replacement_path));
    require(s_replaced && std::filesystem::exists(s_replacement_path) &&
        marker && marker->pid == sintra::get_current_pid(),
        "cleanup preserves a live directory that reuses a validated stale name");
    s_domain.reset();
    sintra::detail::private_cleanup_domain_for_test = nullptr;
}

void replace_before_owned_lock(const std::filesystem::path& path)
{
    require(path == s_teardown_path, "normal teardown observes its fixture");
    require(sintra::detail::remove_private_directory_tree(path),
        "remove the original directory before normal teardown locks it");
    require(sintra::detail::create_private_swarm_directory_exclusive(path) ==
        sintra::detail::private_swarm_create_result::created,
        "reuse the normal teardown directory name");
    sintra::run_marker_record_t marker{};
    marker.pid = sintra::get_current_pid();
    require(sintra::write_run_marker(path, marker), "publish live replacement marker");
    s_teardown_replaced = true;
}

void test_name_reuse_before_normal_teardown()
{
    s_teardown_path = make_stale_directory(0x44a1000000000002ull, std::nullopt);
    s_teardown_replaced = false;
    sintra::detail::before_owned_swarm_lock_for_test = replace_before_owned_lock;
    sintra::cleanup_owned_swarm_directory(s_teardown_path);
    sintra::detail::before_owned_swarm_lock_for_test = nullptr;
    const auto marker = sintra::read_run_marker(sintra::run_marker_path(s_teardown_path));
    require(s_teardown_replaced && std::filesystem::exists(s_teardown_path) &&
        marker && marker->pid == sintra::get_current_pid(),
        "normal teardown preserves a replacement after locking the old inode");
}
#endif

void test_unknown_metadata_startup(int argc, char* argv[])
{
    s_domain.reset();
    sintra::detail::private_cleanup_domain_for_test = injected_domain;
    sintra::init(argc, argv);
    const auto session = std::filesystem::path(sintra::s_mproc->m_directory);
    require(!std::filesystem::exists(session / sintra::detail::private_cleanup_domain_filename()),
        "unknown metadata omits the optional sidecar");
    sintra::detail::finalize();
    sintra::detail::private_cleanup_domain_for_test = nullptr;
}

int unsupported_directory_lock(int, int) { errno = EOPNOTSUPP; return -1; }
int unavailable_directory_lock(int, int) { errno = ENOLCK; return -1; }

void test_absent_metadata_startup(int argc, char* argv[])
{
    s_domain = Cleanup_domain{"ae04ea3d-98b5-4b0d-86ad-856a29b52bd4",
        {Process_metadata_state::VALID, 6, 4101},
        {Process_metadata_state::ABSENT, 0, 0}};
    sintra::detail::private_cleanup_domain_for_test = injected_domain;
    sintra::detail::private_directory_flock_for_test = unsupported_directory_lock;
    sintra::init(argc, argv);
    const auto session = std::filesystem::path(sintra::s_mproc->m_directory);
    require(session == sintra::detail::private_swarm_root(sintra::s_mproc->m_swarm_id),
        "unsupported directory locks use the distinguishable fallback name");
    require(std::filesystem::exists(session / sintra::detail::private_cleanup_domain_filename()),
        "positively absent metadata permits normal startup and sidecar publication");
    sintra::detail::finalize();
    require(!std::filesystem::exists(session), "unsupported locking still permits normal teardown");
    sintra::detail::private_directory_flock_for_test = nullptr;
    s_domain.reset();
    sintra::detail::private_cleanup_domain_for_test = nullptr;
}

void test_unavailable_lock_startup(int argc, char* argv[])
{
    sintra::detail::private_directory_flock_for_test = unavailable_directory_lock;
    sintra::init(argc, argv);
    const auto session = std::filesystem::path(sintra::s_mproc->m_directory);
    require(session == sintra::detail::private_swarm_root(sintra::s_mproc->m_swarm_id),
        "unavailable optional lock capability selects distinct fallback");
    sintra::detail::finalize();
    require(!std::filesystem::exists(session), "unknown lock failure does not disable own teardown");
    sintra::detail::private_directory_flock_for_test = nullptr;
}

void clean_before_creator_lock(const std::filesystem::path& path)
{
    sintra::detail::before_private_lease_lock_for_test = nullptr;
    scan(path.parent_path());
    require(!std::filesystem::exists(path), "scanner can reclaim unpublished unheld directory");
}

void collide_before_creator_lock(const std::filesystem::path&)
{
    sintra::detail::before_private_lease_lock_for_test = nullptr;
    require(sintra::detail::create_private_swarm_directory_exclusive(
        sintra::detail::private_swarm_root(s_first_id)) == sintra::detail::private_swarm_create_result::created,
        "alternate policy creates same ID between collision checks");
}

sintra::detail::Private_directory_lease s_contending_lease;
void replace_before_creator_open(const std::filesystem::path& path)
{
    sintra::detail::before_private_lease_open_for_test = nullptr;
    scan(path.parent_path());
    require(!std::filesystem::exists(path), "scanner removes first allocation before creator opens");
    require(sintra::detail::create_private_swarm_directory_exclusive(path) ==
        sintra::detail::private_swarm_create_result::created, "replacement creator reuses exact name");
    require(s_contending_lease.acquire(path) == sintra::detail::Directory_lease_result::acquired,
        "replacement creator holds its own inode lease");
}

void test_allocation_races(int argc, char* argv[])
{
    s_first_id = 0x55a1000000000101ull;
    s_second_id = 0x55a1000000000102ull;
    s_draws = 0;
    sintra::detail::draw_private_swarm_id_for_test = injected_id;
    sintra::detail::before_private_lease_lock_for_test = clean_before_creator_lock;
    sintra::init(argc, argv);
    require(s_draws == 2 && sintra::s_mproc->m_swarm_id == s_second_id,
        "creator retries rather than publishing into an unlinked directory");
    sintra::detail::finalize();
    s_first_id += 2;
    s_second_id += 2;
    s_draws = 0;
    sintra::detail::before_private_lease_lock_for_test = collide_before_creator_lock;
    sintra::init(argc, argv);
    require(s_draws == 2 && sintra::s_mproc->m_swarm_id == s_second_id &&
        !std::filesystem::exists(sintra::detail::private_swarm_root(s_first_id, true)) &&
        std::filesystem::exists(sintra::detail::private_swarm_root(s_first_id)),
        "alternate-policy collision after allocation forces redraw without deleting contender");
    sintra::detail::finalize();
    s_first_id += 2;
    s_second_id += 2;
    s_draws = 0;
    sintra::detail::before_private_lease_open_for_test = replace_before_creator_open;
    sintra::init(argc, argv);
    const auto replacement = sintra::detail::private_swarm_root(s_first_id, true);
    require(s_draws == 2 && sintra::s_mproc->m_swarm_id == s_second_id &&
        std::filesystem::exists(replacement), "failed acquisition cannot unlink another holder's replacement");
    sintra::detail::finalize();
    sintra::cleanup_owned_swarm_directory(replacement, s_contending_lease.owned_handle());
    s_contending_lease.close();
    sintra::detail::draw_private_swarm_id_for_test = nullptr;
}

void test_failed_creation(const std::filesystem::path& temp_directory)
{
    const auto path = sintra::detail::private_swarm_root(0x55a1000000000901ull, true);
    const auto previous = ::umask(0777);
    const auto result = sintra::detail::create_private_swarm_directory_exclusive(path, nullptr, true);
    ::umask(previous);
    require(result == sintra::detail::private_swarm_create_result::failed &&
        !std::filesystem::exists(path), "privacy validation failure removes its new empty directory");
}

void test_lease_lifetime(const std::filesystem::path& temp_directory)
{
    const auto path = sintra::detail::private_swarm_root(0x55a1000000000001ull, true);
    require(sintra::detail::create_private_swarm_directory_exclusive(path) ==
        sintra::detail::private_swarm_create_result::created, "create lease fixture");
    sintra::detail::Private_directory_lease lease;
    require(lease.acquire(path) == sintra::detail::Directory_lease_result::acquired,
        "native directory lifetime lock is available");
    s_domain.reset();
    sintra::detail::private_cleanup_domain_for_test = injected_domain;
    scan(temp_directory);
    require(std::filesystem::exists(path), "live lifetime lock survives without namespace metadata");
    int ready[2], release[2];
    require(::pipe(ready) == 0 && ::pipe(release) == 0, "create ordinary child gates");
    const int inherited_fd = lease.owned_handle();
    const auto child = ::fork();
    require(child >= 0, "fork with a lifetime lease");
    if (child == 0) {
        ::close(ready[0]);
        ::close(release[1]);
        ::alarm(10);
        errno = 0;
        require(::fcntl(inherited_fd, F_GETFD) == -1 && errno == EBADF,
            "ordinary fork actually closes the inherited descriptor");
        scan(temp_directory);
        require(std::filesystem::exists(path), "fork child close leaves parent lease locked");
        const char byte = 1;
        require(::write(ready[1], &byte, 1) == 1, "publish ordinary child readiness");
        char released = 0;
        (void)::read(release[0], &released, 1);
        ::_exit(0);
    }
    ::close(ready[1]);
    ::close(release[0]);
    char byte = 0;
    require(::read(ready[0], &byte, 1) == 1, "ordinary child remains alive after dropping its lease");
    ::close(ready[0]);
    for (unsigned i = 0; i < 1100; ++i) {
        require(sintra::detail::write_private_file(path / ("backing-" + std::to_string(i)), ""),
            "seed more than 1024 private files");
    }
    lease.close();
    scan(temp_directory);
    require(!std::filesystem::exists(path),
        "unheld lease directory and all files are reclaimed while ordinary child remains alive");
    ::close(release[1]);
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "ordinary child exits after cleanup");
    sintra::detail::private_cleanup_domain_for_test = nullptr;
}

#if defined(__linux__)
void test_real_namespaces(const std::filesystem::path& temp_directory, const char* executable)
{
    const auto path = sintra::detail::private_swarm_root(0x55a1000000000002ull, true);
    require(sintra::detail::create_private_swarm_directory_exclusive(path) ==
        sintra::detail::private_swarm_create_result::created, "create namespace fixture");
    sintra::detail::Private_directory_lease lease;
    require(lease.acquire(path) == sintra::detail::Directory_lease_result::acquired,
        "lock namespace fixture");
    const auto scan_elsewhere = [&](bool probe = false) {
        const auto child = ::fork();
        require(child >= 0, "fork namespace scanner");
        if (child == 0) {
            ::execlp("unshare", "unshare", "--user", "--map-current-user", "--pid", "--time",
                "--fork", executable, probe ? "--namespace-probe" : "--cleanup-scan", temp_directory.c_str(), nullptr);
            ::_exit(77);
        }
        int status = 0;
        require(::waitpid(child, &status, 0) == child && WIFEXITED(status),
            "namespace scanner exits normally");
        return WEXITSTATUS(status);
    };
    const auto result = scan_elsewhere(true);
    if (result != 0) {
        std::fprintf(stderr, "private_swarm_root_test: namespace creation unavailable (exit %d)\n", result);
        sintra::cleanup_owned_swarm_directory(path, lease.owned_handle());
        return;
    }
    require(scan_elsewhere() == 0, "available namespace scanner executes successfully");
    require(std::filesystem::exists(path), "real PID/time namespace scanner preserves live owner");
    lease.close();
    require(scan_elsewhere() == 0 && !std::filesystem::exists(path),
        "real PID/time namespace scanner reclaims abandoned lease");
    std::fprintf(stderr, "private_swarm_root_test: real PID/time namespace checks executed\n");
}

void test_raw_fork_retention(const std::filesystem::path& temp_directory)
{
    const auto path = sintra::detail::private_swarm_root(0x55a1000000000003ull, true);
    require(sintra::detail::create_private_swarm_directory_exclusive(path) ==
        sintra::detail::private_swarm_create_result::created, "create raw fork fixture");
    sintra::detail::Private_directory_lease lease;
    require(lease.acquire(path) == sintra::detail::Directory_lease_result::acquired, "lock raw fork fixture");
    int release[2];
    require(::pipe(release) == 0, "create raw fork gate");
#ifdef SYS_fork
    const auto child = ::syscall(SYS_fork);
#else
    const auto child = ::syscall(SYS_clone, SIGCHLD, nullptr, nullptr, nullptr, 0);
#endif
    require(child >= 0, "raw fork retains descriptor without atfork");
    if (child == 0) {
        ::close(release[1]);
        char byte = 0;
        (void)::read(release[0], &byte, 1);
        lease.~Private_directory_lease();
        // The copied registry now points at storage whose lease lifetime ended.
        // A later ordinary fork must not traverse that foreign registry.
        auto* reused = ::new (static_cast<void*>(&lease)) std::array<unsigned char, sizeof(lease)>;
        reused->fill(0xa5);
        ::alarm(3);
        const auto ordinary = ::fork();
        if (ordinary == 0) { ::_exit(0); }
        int ordinary_status = 0;
        const bool ok = ordinary > 0 && ::waitpid(ordinary, &ordinary_status, 0) == ordinary &&
            WIFEXITED(ordinary_status) && WEXITSTATUS(ordinary_status) == 0;
        ::_exit(ok ? 0 : 1);
    }
    ::close(release[0]);
    lease.close();
    scan(temp_directory);
    require(std::filesystem::exists(path), "raw inherited reference conservatively retains directory");
    ::close(release[1]);
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "raw child can destroy its copied lease and then fork without touching foreign registry");
    scan(temp_directory);
    require(!std::filesystem::exists(path), "last raw reference exit permits reclamation");
}
#endif

void test_coordinator_crash(const std::filesystem::path& temp_directory, int argc, char* argv[])
{
    int ready[2];
    require(::pipe(ready) == 0, "create crash fixture pipe");
    const auto child = ::fork();
    require(child >= 0, "fork crashing coordinator");
    if (child == 0) {
        ::close(ready[0]);
        s_domain.reset();
        sintra::detail::private_cleanup_domain_for_test = injected_domain;
        sintra::init(argc, argv);
        const auto id = sintra::s_mproc->m_swarm_id;
        require(::write(ready[1], &id, sizeof(id)) == sizeof(id), "publish actual initialized swarm id");
        ::_exit(0); // No destructors, signal handlers, or finalize.
    }
    ::close(ready[1]);
    std::uint64_t id = 0;
    require(::read(ready[0], &id, sizeof(id)) == sizeof(id), "read crashed swarm id");
    ::close(ready[0]);
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "observe abrupt coordinator exit");
    const auto path = sintra::detail::private_swarm_root(id, true);
    require(std::filesystem::exists(path), "abrupt exit leaves real ring resources");
    s_domain.reset();
    sintra::detail::private_cleanup_domain_for_test = injected_domain;
    sintra::init(argc, argv);
    require(!std::filesystem::exists(path), "next startup reclaims crashed rings without metadata");
    sintra::detail::finalize();
    sintra::detail::private_cleanup_domain_for_test = nullptr;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 3 && std::string(argv[1]) == "--namespace-probe") { return 0; }
    if (argc == 3 && std::string(argv[1]) == "--cleanup-scan") {
        scan(argv[2]);
        return 0;
    }
    const auto child = ::fork();
    require(child >= 0, "fork stale marker owner");
    if (child == 0) { ::_exit(0); }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status),
        "reap stale marker owner");
    s_dead_pid = static_cast<std::uint32_t>(child);
    const auto temp_directory = sintra::test::unique_scratch_directory("private_swarm_root") / "temp";
    std::filesystem::create_directory(temp_directory);
    require(::chmod(temp_directory.c_str(), 01777) == 0,
        "make temporary directory globally writable and sticky");
    Temp_directory_env temp_env(temp_directory);
    test_coordinator_allocation(temp_directory, argc, argv);
    test_domains(temp_directory);
    test_complete_scan(temp_directory);
#if defined(__linux__)
    test_name_reuse_during_cleanup(temp_directory);
    test_name_reuse_before_normal_teardown();
#endif
    test_unknown_metadata_startup(argc, argv);
    test_absent_metadata_startup(argc, argv);
    test_unavailable_lock_startup(argc, argv);
    test_allocation_races(argc, argv);
    test_failed_creation(temp_directory);
    test_lease_lifetime(temp_directory);
#if defined(__linux__)
    test_real_namespaces(temp_directory, std::filesystem::absolute(argv[0]).c_str());
    test_raw_fork_retention(temp_directory);
#endif
    test_coordinator_crash(temp_directory, argc, argv);
    return 0;
}
#else
namespace {
std::uint64_t draw_collision_id()
{
    static unsigned draws = 0;
    return ++draws == 1 ? 0x16a39c082744912eull : 0x16a39c082744912full;
}
}
int main(int argc, char* argv[])
{
    const auto temp = sintra::test::unique_scratch_directory("private_swarm_root");
    _putenv_s("TEMP", temp.string().c_str());
    _putenv_s("TMP", temp.string().c_str());
    const auto guarded_path = sintra::detail::private_swarm_root(0x66a1000000000001ull);
    sintra::test::require_true(sintra::detail::create_private_directory(guarded_path),
        "private_swarm_root_test: ", "create Windows deletion fixture");
    const sintra::detail::Private_directory_identity original(guarded_path);
    {
        sintra::detail::Private_directory_removal guard(guarded_path, &original);
        sintra::test::require_true(guard.valid() && !RemoveDirectoryW(guarded_path.c_str()) &&
            !MoveFileW(guarded_path.c_str(), (temp / "replacement-name").c_str()),
            "private_swarm_root_test: ", "held Windows root cannot be removed or renamed by path");
        sintra::test::require_true(guard.remove(),
            "private_swarm_root_test: ", "delete exact opened root by handle");
    }
    sintra::test::require_true(!std::filesystem::exists(guarded_path) &&
        sintra::detail::create_private_directory(guarded_path),
        "private_swarm_root_test: ", "reuse released directory name");
    sintra::cleanup_owned_swarm_directory(guarded_path, -1, false, &original);
    sintra::test::require_true(std::filesystem::exists(guarded_path),
        "private_swarm_root_test: ", "old ownership identity cannot remove replacement root");
    const auto old_root = sintra::detail::private_swarm_root();
    sintra::test::require_true(sintra::detail::write_private_file(old_root, "obstruction"),
        "private_swarm_root_test: ", "occupy predictable account name");
    const auto collision = sintra::detail::private_swarm_root(0x16a39c082744912eull);
    sintra::test::require_true(sintra::detail::create_private_directory(collision),
        "private_swarm_root_test: ", "create occupied random candidate");
    sintra::detail::draw_private_swarm_id_for_test = draw_collision_id;
    sintra::init(argc, argv);
    const auto path = std::filesystem::path(sintra::s_mproc->m_directory);
    sintra::test::require_true(path == sintra::detail::private_swarm_root(0x16a39c082744912full) &&
        sintra::detail::private_directory_owned(path), "private_swarm_root_test: ",
        "startup redraws occupied name and uses private direct directory");
    sintra::detail::finalize();
    sintra::detail::draw_private_swarm_id_for_test = nullptr;
    sintra::test::require_true(!std::filesystem::exists(path) &&
        std::filesystem::exists(collision) && std::filesystem::exists(old_root),
        "private_swarm_root_test: ", "teardown removes only its own random root");
    return 0;
}
#endif
