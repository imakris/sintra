#include <sintra/sintra.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr std::string_view kEnvSharedDir = "SINTRA_TEST_SHARED_DIR";

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (!value) {
        throw std::runtime_error("SINTRA_TEST_SHARED_DIR is not set");
    }
    return std::filesystem::path(value);
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(kEnvSharedDir.data(), dir.string().c_str());
#else
    setenv(kEnvSharedDir.data(), dir.string().c_str(), 1);
#endif
}

void clear_shared_directory_env()
{
#ifdef _WIN32
    _putenv_s(kEnvSharedDir.data(), "");
#else
    unsetenv(kEnvSharedDir.data());
#endif
}

std::filesystem::path ensure_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (value) {
        return std::filesystem::path(value);
    }

    auto base = std::filesystem::temp_directory_path() / "sintra_tests";
    std::filesystem::create_directories(base);

    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    std::ostringstream oss;
    oss << "rpc_append_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

void write_lines(const std::filesystem::path& file, const std::vector<std::string>& values)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open " + file.string() + " for writing");
    }
    for (const auto& value : values) {
        out << value << '\n';
    }
}

std::vector<std::string> read_lines(const std::filesystem::path& file)
{
    std::vector<std::string> values;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return values;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        values.push_back(line);
    }
    return values;
}

constexpr int kRepetitions = 32;

std::string barrier_name(const char* base, int iteration)
{
    std::ostringstream oss;
    oss << base << '-' << iteration;
    return oss.str();
}

struct Remotely_accessible : sintra::Derived_transceiver<Remotely_accessible> {
    std::string append(const std::string& s, int v)
    {
        if (s.size() > 10) {
            throw std::logic_error("string too long");
        }
        return std::to_string(v) + ": " + s;
    }

    SINTRA_RPC(append)
};

int process_owner()
{
    Remotely_accessible ra;
    ra.assign_name("instance name");

    for (int iteration = 0; iteration < kRepetitions; ++iteration) {
        sintra::barrier(barrier_name("object-ready", iteration));
        sintra::barrier(barrier_name("calls-finished", iteration));
    }
    return 0;
}

int process_client()
{
    struct Test_case {
        std::string city;
        int year;
        bool expect_success;
    };

    const std::vector<Test_case> cases = {
        {"Sydney", 2000, true},
        {"Athens", 2004, true},
        {"Beijing", 2008, true},
        {"Rio de Janeiro", 2016, false}, // triggers "string too long"
    };

    std::vector<std::string> successes;
    successes.reserve(cases.size() * kRepetitions);
    std::vector<std::string> failures;
    failures.reserve(kRepetitions);

    const auto shared_dir = get_shared_directory();

    for (int iteration = 0; iteration < kRepetitions; ++iteration) {
        sintra::barrier(barrier_name("object-ready", iteration));

        for (const auto& tc : cases) {
            try {
                auto value = Remotely_accessible::rpc_append("instance name", tc.city, tc.year);
                successes.push_back(value);
            }
            catch (const std::exception& e) {
                failures.push_back(e.what());
            }
        }

        sintra::barrier(barrier_name("calls-finished", iteration));
    }

    write_lines(shared_dir / "rpc_success.txt", successes);
    write_lines(shared_dir / "rpc_failures.txt", failures);
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });

    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(process_owner);
    processes.emplace_back(process_client);

    sintra::init(argc, argv, processes);
    sintra::finalize();

    int result = 0;
    if (!is_spawned) {
        const auto success_path = shared_dir / "rpc_success.txt";
        const auto failure_path = shared_dir / "rpc_failures.txt";

        const auto successes = read_lines(success_path);
        const auto failures = read_lines(failure_path);

        const std::vector<std::string> expected_successes_single = {
            "2000: Sydney",
            "2004: Athens",
            "2008: Beijing",
        };
        const std::vector<std::string> expected_failures_single = {
            "string too long",
        };

        std::vector<std::string> expected_successes;
        expected_successes.reserve(expected_successes_single.size() * kRepetitions);
        std::vector<std::string> expected_failures;
        expected_failures.reserve(expected_failures_single.size() * kRepetitions);

        for (int i = 0; i < kRepetitions; ++i) {
            expected_successes.insert(expected_successes.end(),
                                      expected_successes_single.begin(),
                                      expected_successes_single.end());
            expected_failures.insert(expected_failures.end(),
                                     expected_failures_single.begin(),
                                     expected_failures_single.end());
        }

        if (successes != expected_successes || failures != expected_failures) {
            result = 1;
        }

        try {
            std::filesystem::remove_all(shared_dir);
        }
        catch (...) {
            // best-effort cleanup; ignore failures
        }
    }

    clear_shared_directory_env();
    return result;
}
