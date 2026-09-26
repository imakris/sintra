#include <sintra/detail/ipc/file_utils.h>
#include <sintra/detail/ipc/semaphore.h>

#include "test_utils.h"

#include <array>
#include <cstdio>
#include <fstream>

#ifdef _WIN32
#include <aclapi.h>
#endif

namespace {

#ifdef _WIN32
bool has_private_descriptor(HANDLE object, SE_OBJECT_TYPE type)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    alignas(TOKEN_USER) std::array<std::byte, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> user{};
    DWORD needed = 0;
    const bool user_read = GetTokenInformation(
        token, TokenUser, user.data(), static_cast<DWORD>(user.size()), &needed) != 0;
    CloseHandle(token);
    if (!user_read) {
        return false;
    }

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto error = GetSecurityInfo(object, type,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    bool ok = error == ERROR_SUCCESS && dacl && dacl->AceCount == 1;
    if (ok) {
        void* entry = nullptr;
        ok = GetAce(dacl, 0, &entry) != 0;
        if (ok) {
            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
            const auto sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
            ok = ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
                EqualSid(owner, sid) && EqualSid(const_cast<DWORD*>(&ace->SidStart), sid);
        }
    }
    if (descriptor) {
        LocalFree(descriptor);
    }
    return ok;
}
#endif

bool private_backing_file(const std::filesystem::path& directory)
{
    const auto path = directory / "private_backing";
    const auto file = sintra::detail::create_new_file(path.string().c_str());
    if (file == sintra::detail::invalid_file()) {
        return false;
    }
#ifdef _WIN32
    const bool private_access = has_private_descriptor(file, SE_FILE_OBJECT);
#else
    struct stat metadata{};
    const bool private_access = fstat(file, &metadata) == 0 &&
        metadata.st_uid == geteuid() && (metadata.st_mode & 0777) == 0600;
#endif
    sintra::detail::close_file(file);
    return private_access;
}

bool private_semaphore()
{
#ifdef _WIN32
    const auto name = L"Local\\SintraPrivateTest_" + std::to_wstring(GetCurrentProcessId());
    sintra::detail::interprocess_semaphore semaphore(0, name.c_str());
    semaphore.post();
    const auto handle = OpenSemaphoreW(READ_CONTROL, FALSE, name.c_str());
    if (!handle) {
        return false;
    }
    const bool private_access = has_private_descriptor(handle, SE_KERNEL_OBJECT);
    CloseHandle(handle);

    // This object belongs to this fixture; inspecting it requires no other account.
    sintra::detail::Private_security fixture_security;
    if (!SetSecurityDescriptorDacl(fixture_security.attributes()->lpSecurityDescriptor,
            TRUE, nullptr, FALSE))
    {
        return false;
    }
    const auto permissive_name = name + L"_permissive";
    const auto permissive = CreateSemaphoreW(fixture_security.attributes(), 1, 1,
        permissive_name.c_str());
    if (!permissive) {
        return false;
    }
    sintra::detail::interprocess_semaphore rejected(0, permissive_name.c_str());
    const bool refused = !rejected.try_wait() && errno == EINVAL;
    const bool token_retained = WaitForSingleObject(permissive, 0) == WAIT_OBJECT_0;
    CloseHandle(permissive);
    return private_access && semaphore.try_wait() && refused && token_retained;
#else
    // POSIX semaphore state lives in its backing mapping, with no named object.
    return true;
#endif
}

void require(bool ok, const char* message)
{
    sintra::test::require_true(ok, "private_ipc_resources_test: ", message);
}

void set_fixture_private(const std::filesystem::path& path, bool private_access, bool directory = false)
{
#ifdef _WIN32
    sintra::detail::Private_security security(directory);
    PACL acl = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    if (private_access) {
        require(GetSecurityDescriptorDacl(security.attributes()->lpSecurityDescriptor,
            &present, &acl, &defaulted) != 0, "read fixture descriptor");
    }
    const auto handle = CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open owned fixture to set its ACL");
    const auto error = SetSecurityInfo(handle, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    CloseHandle(handle);
    if (error != ERROR_SUCCESS) {
        std::fprintf(stderr, "fixture ACL error=%lu private=%d path=%s\n",
            error, private_access, path.string().c_str());
    }
    require(error == ERROR_SUCCESS, "set owned fixture ACL");
#else
    require(chmod(path.c_str(), directory ? (private_access ? 0700 : 0755) :
        (private_access ? 0600 : 0666)) == 0, "set owned fixture mode");
#endif
}

void test_attachment_policy(const std::filesystem::path& directory)
{
    const auto file = directory / "caller_file";
    require(sintra::detail::write_private_file(file, "private"), "create fixture file");
    set_fixture_private(file, false);
    require(!sintra::detail::private_file_path_owned(file), "reject permissive backing file");
    {
        sintra::ipc::file_mapping generic(file, sintra::ipc::read_only);
        require(generic.size() == 7, "generic caller-controlled mappings retain access policy");
    }
    set_fixture_private(file, true);
    const auto link = directory / "file_link";
    std::error_code ec;
    std::filesystem::create_symlink(file, link, ec);
    if (!ec) {
        sintra::ipc::file_mapping generic(link, sintra::ipc::read_only);
        require(generic.size() == 7, "generic mapping still follows caller links");
        require(!sintra::detail::private_file_path_owned(link), "managed backing rejects links");
    }
    else {
        std::printf("LINK_FIXTURE unavailable: %s\n", ec.message().c_str());
    }

    const auto capacity = sintra::aligned_capacity<uint32_t>(128);
    sintra::Ring_W<uint32_t> writer(directory.string(), "owned_ring", capacity);
    const auto control = directory / "owned_ring_control";
    set_fixture_private(control, false);
    bool rejected = false;
    try {
        sintra::Ring_R<uint32_t> reader(directory.string(), "owned_ring", capacity);
    }
    catch (const sintra::ring_acquisition_failure_exception&) {
        rejected = true;
    }
    set_fixture_private(control, true);
    require(rejected, "actual ring attachment rejects permissive control file");
    sintra::Ring_R<uint32_t> reader(directory.string(), "owned_ring", capacity);
}

void test_cleanup_policy(const std::filesystem::path& directory)
{
    const auto collision = directory / "directory_collision";
    require(sintra::detail::write_private_file(collision, "preserve"), "create owned path collision");
    require(!sintra::detail::create_private_directory(collision), "never replace a regular file with a directory");
    require(std::filesystem::file_size(collision) == 8, "directory collision leaves file intact");
    const auto root = directory / "cleanup";
    require(sintra::detail::create_private_directory(root), "create private cleanup root");
    const auto stale = root / "stale";
    const auto retained = root / "retained";
    const auto mixed = root / "mixed";
    for (const auto& child : {stale, retained, mixed}) {
        require(sintra::detail::create_private_directory(child), "create private session fixture");
        require(sintra::detail::write_private_file(sintra::run_marker_path(child), "pid=invalid\n"),
            "create stale fixture marker");
    }
    set_fixture_private(retained, false, true);
    set_fixture_private(sintra::run_marker_path(mixed), false);
    require(!sintra::detail::create_private_directory(retained), "never adopt permissive directory");

    set_fixture_private(root, false, true);
    sintra::cleanup_stale_swarm_directories(root, sintra::get_current_pid(),
        sintra::current_process_start_stamp().value_or(0));
    require(std::filesystem::exists(stale), "never scavenge an untrusted root");
    set_fixture_private(root, true, true);

    const auto target = directory / "link_target";
    require(sintra::detail::create_private_directory(target), "create owned link target");
    require(sintra::detail::write_private_file(sintra::run_marker_path(target), "pid=invalid\n"),
        "create owned link target marker");
    const auto link = root / "linked";
    std::error_code link_error;
    std::filesystem::create_directory_symlink(target, link, link_error);
    if (!link_error) {
        require(!sintra::detail::create_private_directory(link), "never adopt a linked session");
    }
    else {
        std::printf("DIRECTORY_LINK_FIXTURE unavailable: %s\n", link_error.message().c_str());
    }
    sintra::cleanup_stale_swarm_directories(root, sintra::get_current_pid(),
        sintra::current_process_start_stamp().value_or(0));
    require(!std::filesystem::exists(stale), "private stale session removed");
    require(std::filesystem::exists(sintra::run_marker_path(retained)), "untrusted directory retained");
    require(std::filesystem::exists(mixed), "untrusted file prevents recursive cleanup");
    require(std::filesystem::exists(sintra::run_marker_path(target)), "cleanup leaves link target intact");
    if (!link_error) {
        require(std::filesystem::remove(link), "remove only the owned link fixture");
    }
    set_fixture_private(retained, true, true);
    set_fixture_private(sintra::run_marker_cleanup_path(mixed), true);
    require(sintra::detail::remove_private_directory_tree(root), "remove restored owned fixtures");
}

void test_managed_session(int argc, char* argv[])
{
    sintra::init(argc, argv);
    const std::filesystem::path session = sintra::s_mproc->m_directory;
    require(session.parent_path() == sintra::detail::private_swarm_root(), "per-account root selected");
    require(sintra::detail::private_directory_owned(session.parent_path()), "root is private");
    require(sintra::detail::private_directory_owned(session), "session is private");
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(session)) {
        require(sintra::detail::private_file_path_owned(entry.path()), "every managed file is private");
        ++files;
    }
    require(files >= 8, "inspect real ring data/control/anchor and lifecycle markers");
    sintra::detail::finalize();
    require(!std::filesystem::exists(session), "finalize removes private session");
    std::printf("MANAGED_PRIVATE files=%zu\n", files);
}

} // namespace

int main(int argc, char* argv[])
{
    const auto directory = sintra::test::unique_scratch_directory("private_ipc") / "private";
    require(sintra::detail::create_private_directory(directory), "create private fixture enclosure");
    const bool file_private = private_backing_file(directory);
    const bool semaphore_private = private_semaphore();
    std::printf("PRIVATE_IPC file=%d semaphore=%d\n", file_private, semaphore_private);
    test_attachment_policy(directory);
    test_cleanup_policy(directory);
    test_managed_session(argc, argv);
    return file_private && semaphore_private ? 0 : 1;
}
