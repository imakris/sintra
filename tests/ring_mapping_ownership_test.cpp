#include "isolated_test_support.h"
#include "ring_fixture_test_support.h"

#include "test_ring_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Fail the actual mapped_region allocation immediately after its preceding hook.
thread_local bool s_fail_mapping_allocation = false;
thread_local unsigned s_allocation_failures = 0;

// Arm immediately before the actual allocating expression under test. The
// constructor must unwind the resulting allocator failure, rather than swallow
// a throwing observation and continue successfully.
thread_local bool s_fail_next_allocation = false;
thread_local unsigned s_successful_allocations_before_failure = 0;

void* operator new(std::size_t bytes)
{
    if (s_fail_mapping_allocation) {
        s_fail_mapping_allocation = false;
        ++s_allocation_failures;
        throw std::bad_alloc();
    }
    if (s_fail_next_allocation) {
        if (s_successful_allocations_before_failure != 0) {
            --s_successful_allocations_before_failure;
        }
        else {
            s_fail_next_allocation = false;
            throw std::bad_alloc();
        }
    }
    if (void* memory = std::malloc(bytes ? bytes : 1)) { return memory; }
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

namespace hooks = sintra::detail::test_hooks;
constexpr unsigned k_attempts = 8;
constexpr unsigned char k_marker = 0xa7;
constexpr size_t k_bytes = sintra::message_ring_size;

void require(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}

void* acquire_exact(void* address)
{
#if defined(_WIN32)
    return ::VirtualAlloc(address, k_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__linux__) && defined(MAP_FIXED_NOREPLACE)
    flags |= MAP_FIXED_NOREPLACE;
#elif defined(__FreeBSD__) && defined(MAP_EXCL)
    flags |= MAP_FIXED | MAP_EXCL;
#endif
    void* mapped = ::mmap(address, k_bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (mapped == MAP_FAILED) { return nullptr; }
    if (mapped != address) {
        ::munmap(mapped, k_bytes);
        return nullptr;
    }
    return mapped;
#endif
}

bool release(void* address)
{
#if defined(_WIN32)
    return ::VirtualFree(address, 0, MEM_RELEASE) != 0;
#else
    return ::munmap(address, k_bytes) == 0;
#endif
}

bool present(void* address)
{
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION info{};
    return ::VirtualQuery(address, &info, sizeof(info)) == sizeof(info) &&
        info.State == MEM_COMMIT && info.AllocationBase == address && info.RegionSize >= k_bytes;
#else
    // FreeBSD mprotect can succeed across holes; mincore must cover every page.
#if defined(__linux__)
    std::vector<unsigned char> residency(k_bytes / sintra::system_page_size());
#else
    std::vector<char> residency(k_bytes / sintra::system_page_size());
#endif
    return ::mincore(address, k_bytes, residency.data()) == 0;
#endif
}

enum class Fault { FIRST_ALLOCATION, SECOND_ALLOCATION, NATIVE_FAILURE, COLLISION };

class Mapping_probe
{
public:
    explicit Mapping_probe(Fault fault) : m_fault(fault), m_reuser([this] { run_reuser(); })
    {
        // Allocate the native thread stack before any mapping interval is
        // released. Creating a thread in the hook could consume that vacancy.
        {
            std::unique_lock lock(m_mutex);
            m_condition.wait(lock, [&] { return m_reuser_ready; });
        }
        s_active = this;
        s_allocation_failures = 0;
        hooks::s_ring_mapping_operation = callback;
        if (fault == Fault::NATIVE_FAILURE) {
#if defined(__linux__) && defined(MAP_SYNC) && defined(MAP_SHARED_VALIDATE)
            hooks::s_ring_mapping_options = MAP_SHARED_VALIDATE | MAP_SYNC;
#elif defined(__APPLE__) && defined(MAP_JIT)
            hooks::s_ring_mapping_options = MAP_JIT;
#endif
        }
    }

    ~Mapping_probe()
    {
        disarm();
        for (auto* address : m_replacements) {
            if (address) { release(address); }
        }
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_condition.notify_one();
        m_reuser.join();
    }

    void verify(bool failed)
    {
        disarm();
        require(!m_error, m_error ? m_error : "mapping fixture failed");
        require(failed && m_attempts == k_attempts, "acquisition did not exhaust exactly eight attempts");
        if (m_fault == Fault::FIRST_ALLOCATION || m_fault == Fault::SECOND_ALLOCATION) {
            require(s_allocation_failures == k_attempts, "actual allocation failure was not reached");
        }
        if (m_fault == Fault::NATIVE_FAILURE) {
            require(m_native_failures == k_attempts, "actual native mapping failure was not reached");
        }
        require(m_replacement_count == k_attempts, "another thread did not acquire each released interval");
        if (m_fault == Fault::COLLISION) {
            require(m_vacant_first_views == k_attempts, "collision cleanup retained a first view");
#if defined(_WIN32) || (defined(__FreeBSD__) && defined(MAP_EXCL))
            require(m_native_failures == k_attempts, "collision did not reach native exact-placement refusal");
#elif defined(__linux__) && defined(MAP_FIXED_NOREPLACE)
            // Older kernels can ignore NOREPLACE and honor only the hint. Probe
            // an owned occupied interval before requiring native refusal.
            void* probe = ::mmap(m_replacements[0], k_bytes, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (probe == MAP_FAILED) {
                require(errno == EEXIST, "native collision capability probe failed unexpectedly");
                require(m_native_failures == k_attempts,
                    "collision did not reach native exact-placement refusal");
            }
            else {
                const bool relocated = probe != m_replacements[0];
                require(::munmap(probe, k_bytes) == 0, "collision probe release failed");
                require(relocated, "collision probe found a lost replacement mapping");
            }
#endif
        }
        bool preserved = true;
        for (auto& address : m_replacements) {
            const bool mapped = present(address);
            if (mapped) {
                const auto* bytes = static_cast<const unsigned char*>(address);
                preserved &= bytes[0] == k_marker && bytes[k_bytes - 1] == k_marker;
                require(release(address), "replacement release failed");
            }
            preserved &= mapped;
            address = nullptr;
        }
        require(preserved, "failed mapping cleanup destroyed another thread's replacement payload");
        std::printf("attempts=%u allocations=%u native_failures=%u replacements=%u preserved=yes\n",
            m_attempts, s_allocation_failures, m_native_failures, m_replacement_count);
    }

private:
    void disarm()
    {
        hooks::s_ring_mapping_operation = nullptr;
        hooks::s_ring_mapping_options = 0;
        s_fail_mapping_allocation = false;
        s_active = nullptr;
    }

    void reuse(void* address)
    {
        if (m_replacement_count == k_attempts) {
            m_error = "unexpected extra mapping attempt";
            return;
        }
        std::unique_lock lock(m_mutex);
        m_requested_address = address;
        m_request_pending = true;
        m_condition.notify_one();
        m_condition.wait(lock, [&] { return !m_request_pending; });
        if (!m_replacement) { m_error = "released interval could not be acquired without replacement"; }
        else { m_replacements[m_replacement_count++] = m_replacement; }
    }

    void run_reuser()
    {
        std::unique_lock lock(m_mutex);
        m_reuser_ready = true;
        m_condition.notify_one();
        for (;;) {
            m_condition.wait(lock, [&] { return m_request_pending || m_stop; });
            if (m_stop) { return; }
            void* replacement = acquire_exact(m_requested_address);
            if (replacement) {
                auto* bytes = static_cast<unsigned char*>(replacement);
                bytes[0] = bytes[k_bytes - 1] = k_marker;
            }
            m_replacement = replacement;
            m_request_pending = false;
            m_condition.notify_one();
        }
    }

    static void callback(const char* stage, const void* object, int error)
    {
        auto& self = *s_active;
        const std::string_view name(stage);
        if (name == "before_first_mapping") {
            ++self.m_attempts;
            self.m_first_address = const_cast<void*>(object);
            if (self.m_fault == Fault::FIRST_ALLOCATION) { s_fail_mapping_allocation = true; }
        }
        else if (name == "first_mapping") {
            const auto* region = static_cast<const sintra::detail::ipc::mapped_region*>(object);
            self.m_first_address = const_cast<void*>(region->data());
            if (self.m_fault == Fault::SECOND_ALLOCATION) { s_fail_mapping_allocation = true; }
            if (self.m_fault == Fault::COLLISION) {
                self.reuse(static_cast<char*>(self.m_first_address) + k_bytes);
            }
        }
        else if (name == "native_failed") {
            ++self.m_native_failures;
            if (self.m_fault == Fault::NATIVE_FAILURE) {
#if defined(__linux__)
                if (error != EOPNOTSUPP) { self.m_error = "native mapping did not fail with EOPNOTSUPP"; }
#elif defined(__APPLE__)
                if (error != EINVAL) { self.m_error = "native mapping did not fail with EINVAL"; }
#endif
                // Reuse before ANY remaining failed-attempt cleanup. Moving this
                // to before_retry would miss master's broad unmap defect.
                self.reuse(const_cast<void*>(object));
            }
        }
        else if (name == "before_retry") {
            if (self.m_fault == Fault::FIRST_ALLOCATION || self.m_fault == Fault::SECOND_ALLOCATION) {
                self.reuse(self.m_first_address);
            }
            if (self.m_fault == Fault::COLLISION) {
                void* vacancy = acquire_exact(self.m_first_address);
                if (vacancy && release(vacancy)) { ++self.m_vacant_first_views; }
                else { self.m_error = "failed second mapping retained its first view"; }
            }
        }
    }

    static inline thread_local Mapping_probe* s_active = nullptr;
    Fault m_fault;
    std::array<void*, k_attempts> m_replacements{};
    void* m_first_address = nullptr;
    const char* m_error = nullptr;
    unsigned m_attempts = 0;
    unsigned m_native_failures = 0;
    unsigned m_replacement_count = 0;
    unsigned m_vacant_first_views = 0;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    void* m_requested_address = nullptr;
    void* m_replacement = nullptr;
    bool m_reuser_ready = false;
    bool m_request_pending = false;
    bool m_stop = false;
    std::thread m_reuser;
};

void delivery(const std::string& directory)
{
    constexpr size_t block_size = k_bytes / 16 + 1;
    std::vector<unsigned char> values(block_size);
    for (unsigned generation = 0; generation != 4; ++generation) {
        sintra::Ring_W<unsigned char> writer(directory, "delivery", k_bytes);
        sintra::Ring_R<unsigned char> reader(directory, "delivery", k_bytes, block_size);
        for (unsigned block = 0; block != 35; ++block) {
            for (size_t i = 0; i < values.size(); ++i) {
                values[i] = (unsigned char)(generation + block + i);
            }
            writer.write_commit(values.data(), values.size());
            const auto range = reader.start_reading(values.size());
            const bool equal = range.end - range.begin == (ptrdiff_t)values.size() &&
                std::equal(range.begin, range.end, values.begin());
            reader.done_reading();
            require(equal, "ordinary message-size ring lost committed wraparound payload");
        }
    }
    std::puts("message-size attach/detach/wraparound/delivery: PASS");
}

void run_fault(Fault fault)
{
    sintra::test::Temp_ring_dir directory("mapping_ownership");
    bool failed = false;
    Mapping_probe probe(fault);
    try {
        sintra::Ring_data<unsigned char, false> data(directory.str(), "data", k_bytes,
            sintra::detail::ring_directory_policy::caller_directory);
    }
    catch (const sintra::ring_acquisition_failure_exception&) { failed = true; }
    probe.verify(failed);
    delivery(directory.str());
}

} // namespace

namespace construction_cleanup {

namespace fs = std::filesystem;
using sintra::test::isolated::require;
using sintra::test::isolated::signal_file;
using sintra::test::isolated::case_t;
using sintra::test::ring_fixture::Probe_reader;
using sintra::test::ring_fixture::Probe_writer;
using sintra::test::ring_fixture::k_ring_name;
using sintra::test::ring_fixture::k_payload;

constexpr case_t k_cases[] = {
    {"data_first_mapping_allocation_unwind", "", "construction"},
    {"control_mapping_allocation_unwind", "", "construction"},
    {"control_abi_diagnostic_allocation_unwind", "", "construction"},
    {"data_attach_marker_allocation_cleanup", "", "construction"},
    {"before_release_marker_allocation_cleanup", "", "construction"},
    {"after_release_marker_allocation_cleanup", "", "construction"},
    {"private_named_path_allocation_unwind", "", "construction"},
#if defined(__linux__) && defined(MAP_SYNC) && defined(MAP_SHARED_VALIDATE) && defined(MAP_FIXED_NOREPLACE)
    {"data_native_mapping_failure_reuse", "", "construction"},
#endif
#if defined(__FreeBSD__) && defined(MAP_EXCL)
    {"data_native_mapping_conflict_preservation", "", "construction"},
#endif
#if !defined(_WIN32)
    {"private_opened_fd_allocation_unwind", "", "construction"},
    {"data_second_mapping_allocation_unwind", "", "construction"},
#endif
};

bool s_lifecycle_acquired = false;
int s_opened_directory_fd = -1;
const char* s_construction_stage = nullptr;
unsigned s_construction_faults = 0;
void* s_failed_control_address = nullptr;
size_t s_failed_control_size = 0;

void set_fixture_environment(const char* name, const std::string& value)
{
#if defined(_WIN32)
    require(_putenv_s(name, value.c_str()) == 0, "cannot set construction fixture environment");
#else
    require(::setenv(name, value.c_str(), 1) == 0, "cannot set construction fixture environment");
#endif
}
#if !defined(_WIN32)
void* s_reused_mappings[8]{};
size_t s_reused_mapping_count = 0;
size_t s_mapping_bytes = 0;
#endif

int run_construction_case(const case_t& test, const char* directory)
{
    const std::string_view name = test.name;
    const std::string path(directory);
    const auto elements = sintra::test::pick_ring_elements<uint32_t>();
    bool failed = false;
    if (name == "data_first_mapping_allocation_unwind" || name == "control_mapping_allocation_unwind" ||
        name == "control_abi_diagnostic_allocation_unwind")
    {
        Probe_writer writer(path, k_ring_name, elements);
        const auto slots = writer.free_slots();
        const auto original_abi = name == "control_abi_diagnostic_allocation_unwind" ?
            writer.replace_control_abi(0) : 0;
        s_construction_stage = name == "data_first_mapping_allocation_unwind" ? "data_before_first_mapping" :
            name == "control_mapping_allocation_unwind" ? "control_before_mapping" : "control_abi_diagnostic";
        sintra::detail::s_ring_construction_operation = [](const char* stage, const void* object) {
            if (std::string_view(stage) == s_construction_stage) {
                ++s_construction_faults;
                if (object) {
                    auto* region = static_cast<const sintra::detail::ipc::mapped_region*>(object);
                    s_failed_control_address = const_cast<void*>(region->data());
                    s_failed_control_size = region->size();
                }
                s_fail_next_allocation = true;
            }
        };
        try { Probe_reader reader(path, k_ring_name, elements); }
        catch (const std::exception&) { failed = true; }
        s_fail_next_allocation = false;
        sintra::detail::s_ring_construction_operation = nullptr;
        if (original_abi) { writer.replace_control_abi(original_abi); }
        require(failed && s_construction_faults == (name == "data_first_mapping_allocation_unwind" ? 8u : 1u),
            "actual construction allocation failure did not unwind the requested resource interval");
        if (s_failed_control_address) {
#if defined(_WIN32)
            MEMORY_BASIC_INFORMATION info{};
            require(VirtualQuery(s_failed_control_address, &info, sizeof(info)) == sizeof(info) &&
                info.State == MEM_FREE, "ABI diagnostic allocation leaked its control view");
#elif defined(__FreeBSD__) && defined(MAP_EXCL)
            // FreeBSD mprotect permits unmapped holes; exclusive placement proves vacancy.
            void* released = ::mmap(s_failed_control_address, s_failed_control_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_EXCL, -1, 0);
            require(released == s_failed_control_address,
                "ABI diagnostic allocation leaked its control mapping");
            require(::munmap(released, s_failed_control_size) == 0,
                "ABI diagnostic vacancy probe unmap failed");
#else
            require(::mprotect(s_failed_control_address, s_failed_control_size, PROT_READ | PROT_WRITE) == -1 &&
                errno == ENOMEM, "ABI diagnostic allocation leaked its control mapping");
#endif
        }
        Probe_reader replacement(path, k_ring_name, elements, 1);
        writer.write_commit(k_payload);
        const auto data = replacement.start_reading(1);
        require(data.end - data.begin == 1 && *data.begin == k_payload && writer.free_slots() == slots - 1,
            "constructor unwind damaged committed data or a later real admission");
        replacement.done_reading();
        return 0;
    }
    if (name == "data_attach_marker_allocation_cleanup" || name == "before_release_marker_allocation_cleanup" ||
        name == "after_release_marker_allocation_cleanup")
    {
        const auto data_file = (fs::path(path) / k_ring_name).string();
        const auto marker = (fs::path(path) / "marker_observed").string();
        const auto resume = (fs::path(path) / "marker_resume").string();
        signal_file(resume);
        set_fixture_environment("SINTRA_RING_LIFECYCLE_PAUSE_DATA_FILE", data_file);
        set_fixture_environment("SINTRA_RING_LIFECYCLE_PAUSED_FILE", marker);
        set_fixture_environment("SINTRA_RING_LIFECYCLE_RESUME_FILE", resume);
        set_fixture_environment("SINTRA_RING_LIFECYCLE_RELEASE_DATA_FILE", data_file);
        set_fixture_environment("SINTRA_RING_LIFECYCLE_RELEASE_WAITING_FILE", marker);
        set_fixture_environment("SINTRA_RING_LIFECYCLE_RELEASE_LOCKED_FILE", marker);
        s_construction_stage = name == "data_attach_marker_allocation_cleanup" ? "data_attach_marker" :
            name == "before_release_marker_allocation_cleanup" ? "before_release_marker" : "after_release_marker";
        sintra::detail::s_ring_construction_operation = [](const char* stage, const void*) {
            if (std::string_view(stage) == s_construction_stage) { ++s_construction_faults; s_fail_next_allocation = true; }
        };
        { Probe_reader reader(path, k_ring_name, elements); reader.start_reading(); }
        sintra::detail::s_ring_construction_operation = nullptr;
        s_fail_next_allocation = false;
        require(s_construction_faults == 1 && sintra::detail::test_hooks::take_observation_failure() != nullptr,
            "whole marker boundary did not retain the actual allocation failure");
        require(!fs::exists(data_file) && !fs::exists(data_file + "_control"),
            "marker allocation cancelled mandatory final attachment cleanup");
        Probe_writer replacement(path, k_ring_name, elements);
        replacement.write_commit(k_payload);
        return 0;
    }
    if (name == "private_named_path_allocation_unwind") {
        sintra::detail::native_diagnostic("named path fixture: before construction\n", 0, "", false);
        sintra::detail::test_hooks::s_mutex_operation = [](const char* stage, const void*) {
            if (std::string_view(stage) == "mutex_after_owner_cas") { s_lifecycle_acquired = true; }
        };
        sintra::detail::s_ring_construction_operation = [](const char* stage, const void*) {
            if (s_lifecycle_acquired && std::string_view(stage) == "private_directory_named_path") {
                sintra::detail::native_diagnostic("named path fixture: arm actual allocation\n", 0, "", false);
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
                // MSVC's conversion default-constructs a noexcept wide string
                // with a Debug iterator proxy. Target its throwing resize instead.
                s_successful_allocations_before_failure = 1;
#endif
                s_fail_next_allocation = true;
            }
        };
        try {
            Probe_reader reader(path, k_ring_name, elements, 0,
                sintra::detail::ring_directory_policy::private_existing_directory);
        } catch (const std::exception&) {
            failed = true;
            sintra::detail::native_diagnostic("named path fixture: constructor unwound\n", 0, "", false);
        }
        s_fail_next_allocation = false;
        sintra::detail::s_ring_construction_operation = nullptr;
        sintra::detail::test_hooks::s_mutex_operation = nullptr;
        require(failed && s_lifecycle_acquired, "private path fault did not reach acquired lifecycle unwind");
        sintra::detail::native_diagnostic("named path fixture: before replacement\n", 0, "", false);
        Probe_writer replacement(path, k_ring_name, elements, sintra::detail::ring_directory_policy::private_existing_directory);
        sintra::detail::native_diagnostic("named path fixture: replacement acquired\n", 0, "", false);
        replacement.write_commit(k_payload);
        return 0;
    }
#if !defined(_WIN32)
#if defined(__FreeBSD__) && defined(MAP_EXCL)
    if (name == "data_native_mapping_conflict_preservation") {
        s_mapping_bytes = elements * sizeof(uint32_t);
        sintra::detail::s_ring_construction_operation = [](const char* stage, const void* object) {
            if (std::string_view(stage) == "data_first_mapping") {
                require(s_reused_mapping_count < 8, "unexpected additional native mapping attempt");
                const auto* region = static_cast<const sintra::detail::ipc::mapped_region*>(object);
                void* address = static_cast<char*>(const_cast<void*>(region->data())) + s_mapping_bytes;
                void* replacement = MAP_FAILED;
                std::thread reuser([&] {
                    replacement = ::mmap(address, s_mapping_bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_EXCL, -1, 0);
                    if (replacement != MAP_FAILED) { *static_cast<uint32_t*>(replacement) = k_payload; }
                });
                reuser.join();
                require(replacement == address, "could not occupy the second native mapping interval");
                s_reused_mappings[s_reused_mapping_count++] = replacement;
            }
            if (std::string_view(stage) == "data_mapping_native_failed") {
                // mmap(2) documents EINVAL; the kernel also reports ENOMEM for occupied ranges.
                const int native_error = int(reinterpret_cast<intptr_t>(object));
                require(native_error == EINVAL || native_error == ENOMEM,
                    "native mapping fixture did not reach the occupied exact interval");
                ++s_construction_faults;
            }
            if (std::string_view(stage) == "data_mapping_before_retry") {
                require(s_construction_faults == s_reused_mapping_count,
                    "occupied second interval did not cause a native mapping failure");
                void* released = ::mmap(const_cast<void*>(object), s_mapping_bytes,
                    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_EXCL, -1, 0);
                require(released == object, "failed second mapping retained its acquired first interval");
                require(::munmap(released, s_mapping_bytes) == 0, "released first interval unmap failed");
            }
        };
        try { Probe_reader reader(path, k_ring_name, elements); }
        catch (const sintra::ring_acquisition_failure_exception&) { failed = true; }
        sintra::detail::s_ring_construction_operation = nullptr;
        bool preserved = failed && s_construction_faults == 8 && s_reused_mapping_count == 8;
        for (size_t i = 0; i < s_reused_mapping_count; ++i) {
            const bool mapped = ::mprotect(s_reused_mappings[i], s_mapping_bytes, PROT_READ | PROT_WRITE) == 0;
            preserved = preserved && mapped;
            if (mapped) {
                preserved = preserved && *static_cast<uint32_t*>(s_reused_mappings[i]) == k_payload;
                require(::munmap(s_reused_mappings[i], s_mapping_bytes) == 0, "native conflict mapping unmap failed");
            }
        }
        require(preserved, "failed exact mapping cleanup replaced or unmapped another thread's interval");
        Probe_writer writer(path, k_ring_name, elements);
        Probe_reader reader(path, k_ring_name, elements, 1);
        writer.write_commit(k_payload);
        const auto data = reader.start_reading(1);
        require(data.end - data.begin == 1 && *data.begin == k_payload,
            "native mapping conflict damaged later real admission or committed data");
        reader.done_reading();
        return 0;
    }
#endif
#if defined(__linux__) && defined(MAP_SYNC) && defined(MAP_SHARED_VALIDATE) && defined(MAP_FIXED_NOREPLACE)
    if (name == "data_native_mapping_failure_reuse") {
        s_mapping_bytes = elements * sizeof(uint32_t);
        sintra::detail::s_ring_mapping_options_for_test = MAP_SHARED_VALIDATE | MAP_SYNC;
        sintra::detail::s_ring_construction_operation = [](const char* stage, const void* object) {
            if (std::string_view(stage) == "data_mapping_native_failed") {
                require(int(reinterpret_cast<intptr_t>(object)) == EOPNOTSUPP,
                    "native mapping fixture did not reach the unsupported synchronous file mapper");
                ++s_construction_faults;
            }
            if (std::string_view(stage) == "data_mapping_before_retry") {
                require(s_reused_mapping_count < 8 && s_construction_faults == s_reused_mapping_count + 1,
                    "native mapping failure was not observed before address reuse");
                std::thread reuser([object] {
                    void* replacement = ::mmap(const_cast<void*>(object), s_mapping_bytes,
                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                    require(replacement == object, "failed native mapping did not leave the interval vacant");
                    *static_cast<uint32_t*>(replacement) = k_payload;
                    s_reused_mappings[s_reused_mapping_count++] = replacement;
                });
                reuser.join();
            }
        };
        try { Probe_reader reader(path, k_ring_name, elements); }
        catch (const sintra::ring_acquisition_failure_exception&) { failed = true; }
        sintra::detail::s_ring_construction_operation = nullptr;
        sintra::detail::s_ring_mapping_options_for_test = 0;
        bool preserved = failed && s_construction_faults == 8 && s_reused_mapping_count == 8;
        for (size_t i = 0; i < s_reused_mapping_count; ++i) {
            const bool mapped = ::mprotect(s_reused_mappings[i], s_mapping_bytes, PROT_READ | PROT_WRITE) == 0;
            preserved = preserved && mapped;
            if (mapped) {
                preserved = preserved && *static_cast<uint32_t*>(s_reused_mappings[i]) == k_payload;
                require(::munmap(s_reused_mappings[i], s_mapping_bytes) == 0, "native replacement unmap failed");
            }
        }
        require(preserved, "native mapping failure cleanup unmapped another thread's replacement");
        Probe_writer replacement(path, k_ring_name, elements);
        replacement.write_commit(k_payload);
        return 0;
    }
#endif
    if (name == "private_opened_fd_allocation_unwind") {
        sintra::detail::s_ring_construction_operation = [](const char* stage, const void* object) {
            if (std::string_view(stage) == "private_directory_opened") {
                s_opened_directory_fd = int(reinterpret_cast<intptr_t>(object));
                s_fail_next_allocation = true;
            }
        };
        try {
            sintra::detail::ring_directory held(path,
                sintra::detail::ring_directory_policy::private_existing_directory);
        } catch (const std::bad_alloc&) { failed = true; }
        sintra::detail::s_ring_construction_operation = nullptr;
        s_fail_next_allocation = false;
        errno = 0;
        const int disposition = ::fcntl(s_opened_directory_fd, F_GETFD);
        require(failed && s_opened_directory_fd >= 0 && disposition == -1 && errno == EBADF,
            "failed directory construction leaked its already-open native descriptor");
        return 0;
    }
    if (name == "data_second_mapping_allocation_unwind") {
        s_mapping_bytes = elements * sizeof(uint32_t);
        sintra::detail::s_ring_construction_operation = [](const char* stage, const void* object) {
            if (std::string_view(stage) == "data_first_mapping") {
                s_fail_next_allocation = true;
            }
            if (std::string_view(stage) == "data_mapping_before_retry") {
                require(s_reused_mapping_count < 8, "unexpected additional mapping attempt");
                // A separate thread obtains the released first interval while
                // the failure path still owns its remaining cleanup work.
                std::thread reuser([object] {
                    void* replacement = ::mmap(const_cast<void*>(object), s_mapping_bytes,
                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                    require(replacement == object, "could not reuse the released mapping interval");
                    *static_cast<uint32_t*>(replacement) = k_payload;
                    s_reused_mappings[s_reused_mapping_count++] = replacement;
                });
                reuser.join();
            }
        };
        try { Probe_reader reader(path, k_ring_name, elements); }
        catch (const sintra::ring_acquisition_failure_exception&) { failed = true; }
        sintra::detail::s_ring_construction_operation = nullptr;
        s_fail_next_allocation = false;
        bool preserved = failed && s_reused_mapping_count == 8;
        for (size_t i=0; i<s_reused_mapping_count; ++i) {
            const bool mapped = ::mprotect(s_reused_mappings[i], s_mapping_bytes, PROT_READ | PROT_WRITE) == 0;
            preserved = preserved && mapped;
            if (mapped) {
                preserved = preserved && *static_cast<uint32_t*>(s_reused_mappings[i]) == k_payload;
                require(::munmap(s_reused_mappings[i], s_mapping_bytes) == 0, "replacement unmap failed");
            }
        }
        require(preserved, "partial mapping cleanup unmapped a different thread's replacement interval");
        Probe_writer replacement(path, k_ring_name, elements);
        return 0;
    }
#endif
    throw std::runtime_error("unknown construction case");
}

int run_case(const std::string&, const case_t& test, const char* directory)
{
    return run_construction_case(test, directory);
}

} // namespace construction_cleanup

int main(int argc, char** argv)
{
    if (const auto result = sintra::test::isolated::run_cases(
        argc, argv, construction_cleanup::k_cases, construction_cleanup::run_case, true))
    {
        return *result;
    }
    const std::string_view selected = argc == 3 && std::string_view(argv[1]) == "--case"
        ? argv[2] : "all";
    bool ran = false;
    const auto run = [&](const char* name, auto&& action) {
        if (selected == "all" || selected == name) {
            ran = true;
            action();
            std::printf("%s: PASS\n", name);
        }
    };
    try {
        run("first_allocation", [] { run_fault(Fault::FIRST_ALLOCATION); });
        run("second_allocation", [] { run_fault(Fault::SECOND_ALLOCATION); });
#if (defined(__linux__) && defined(MAP_SYNC) && defined(MAP_SHARED_VALIDATE)) || \
    (defined(__APPLE__) && defined(MAP_JIT))
        run("native_failure", [] { run_fault(Fault::NATIVE_FAILURE); });
#endif
        run("collision", [] { run_fault(Fault::COLLISION); });
        run("delivery", [] {
            sintra::test::Temp_ring_dir directory("mapping_delivery");
            delivery(directory.str());
        });
        require(ran, "unknown or unsupported mapping test case");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "ring_mapping_ownership_test: %s\n", error.what());
        return 1;
    }
}
