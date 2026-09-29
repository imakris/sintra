#include <sintra/sintra.h>
#include <sintra/detail/ipc/process_utils.h>

#include "test_utils.h"

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
    require(session == sintra::detail::private_swarm_root(s_second_id) &&
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

void test_scan_bound(const std::filesystem::path& temp_directory)
{
    Cleanup_domain comparable{"dc82a410-5980-496f-81c9-c977677e1f2d",
        {Process_metadata_state::VALID, 5, 3101},
        {Process_metadata_state::VALID, 5, 3202}};
    s_domain = comparable;
    sintra::detail::private_cleanup_domain_for_test = injected_domain;
    for (std::uint64_t id = 0x33a1000000000001ull; id <= 0x33a1000000000046ull; ++id) {
        (void)make_stale_directory(id, comparable);
    }
    scan(temp_directory);
    std::size_t remaining = 0;
    for (std::uint64_t id = 0x33a1000000000001ull; id <= 0x33a1000000000046ull; ++id) {
        remaining += std::filesystem::exists(sintra::detail::private_swarm_root(id));
    }
    require(remaining >= 6 && remaining < 70,
        "one scan makes progress and examines at most 64 candidates");
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

void test_absent_metadata_startup(int argc, char* argv[])
{
    s_domain = Cleanup_domain{"ae04ea3d-98b5-4b0d-86ad-856a29b52bd4",
        {Process_metadata_state::VALID, 6, 4101},
        {Process_metadata_state::ABSENT, 0, 0}};
    sintra::detail::private_cleanup_domain_for_test = injected_domain;
    sintra::init(argc, argv);
    const auto session = std::filesystem::path(sintra::s_mproc->m_directory);
    require(std::filesystem::exists(session / sintra::detail::private_cleanup_domain_filename()),
        "positively absent metadata permits normal startup and sidecar publication");
    sintra::detail::finalize();
    s_domain.reset();
    sintra::detail::private_cleanup_domain_for_test = nullptr;
}

} // namespace

int main(int argc, char* argv[])
{
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
    test_scan_bound(temp_directory);
#if defined(__linux__)
    test_name_reuse_during_cleanup(temp_directory);
    test_name_reuse_before_normal_teardown();
#endif
    test_unknown_metadata_startup(argc, argv);
    test_absent_metadata_startup(argc, argv);
    return 0;
}
#else
int main() { return 0; }
#endif
