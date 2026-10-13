#include "test_ring_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

// Fail the actual mapped_region allocation immediately after its preceding hook.
thread_local bool s_fail_mapping_allocation = false;
thread_local unsigned s_allocation_failures = 0;

void* operator new(std::size_t bytes)
{
    if (s_fail_mapping_allocation) {
        s_fail_mapping_allocation = false;
        ++s_allocation_failures;
        throw std::bad_alloc();
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

int main(int argc, char** argv)
{
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
