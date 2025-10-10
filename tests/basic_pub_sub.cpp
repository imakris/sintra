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
    oss << "basic_pubsub_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

void write_strings(const std::filesystem::path& file, const std::vector<std::string>& values)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open " + file.string() + " for writing");
    }
    for (const auto& value : values) {
        out << value << '\n';
    }
}

void write_ints(const std::filesystem::path& file, const std::vector<int>& values)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open " + file.string() + " for writing");
    }
    for (int value : values) {
        out << value << '\n';
    }
}

std::vector<std::string> read_strings(const std::filesystem::path& file)
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

std::vector<int> read_ints(const std::filesystem::path& file)
{
    std::vector<int> values;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return values;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            values.push_back(std::stoi(line));
        }
    }
    return values;
}

void write_result(const std::filesystem::path& dir,
                  bool ok,
                  const std::vector<std::string>& strings,
                  const std::vector<int>& ints)
{
    std::ofstream out(dir / "result.txt", std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open result file");
    }
    out << (ok ? "ok" : "fail") << '\n';
    if (!ok) {
        out << "strings:";
        for (const auto& value : strings) {
            out << ' ' << value;
        }
        out << "\nints:";
        for (int value : ints) {
            out << ' ' << value;
        }
        out << '\n';
    }
}

bool has_branch_flag(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}

constexpr int kRepetitions = 32;

std::string barrier_name(const char* base, int iteration)
{
    std::ostringstream oss;
    oss << base << '-' << iteration;
    return oss.str();
}

std::filesystem::path iteration_file(const std::filesystem::path& dir,
                                     const std::string& prefix,
                                     int iteration)
{
    std::ostringstream oss;
    oss << prefix << '_' << iteration << ".txt";
    return dir / oss.str();
}

std::vector<std::string> g_received_strings;
std::vector<int> g_received_ints;

int process_sender()
{
    const auto shared_dir = get_shared_directory();

    const std::vector<std::string> expected_strings{
        "good morning", "good afternoon", "good evening", "good night"};
    const std::vector<int> expected_ints{1, 2, 3, 4};

    bool overall_ok = true;
    std::vector<std::string> failing_strings;
    std::vector<int> failing_ints;

    for (int iteration = 0; iteration < kRepetitions; ++iteration) {
        const auto slots_ready = barrier_name("slots-ready", iteration);
        const auto messages_done = barrier_name("messages-done", iteration);
        const auto write_phase = barrier_name("write-phase", iteration);

        sintra::barrier(slots_ready);

        sintra::world() << std::string("good morning");
        sintra::world() << std::string("good afternoon");
        sintra::world() << std::string("good evening");
        sintra::world() << std::string("good night");

        sintra::world() << 1;
        sintra::world() << 2;
        sintra::world() << 3;
        sintra::world() << 4;

        sintra::barrier(messages_done);
        sintra::barrier(write_phase);

        const auto strings = read_strings(iteration_file(shared_dir, "strings", iteration));
        const auto ints = read_ints(iteration_file(shared_dir, "ints", iteration));

        const bool iteration_ok = (strings == expected_strings) && (ints == expected_ints);
        if (!iteration_ok && overall_ok) {
            overall_ok = false;
            failing_strings = strings;
            failing_ints = ints;
        }
    }

    write_result(shared_dir, overall_ok, failing_strings, failing_ints);
    return 0;
}

int process_string_receiver()
{
    auto string_slot = [](const std::string& value) {
        g_received_strings.push_back(value);
    };
    sintra::activate_slot(string_slot);

    const auto shared_dir = get_shared_directory();

    for (int iteration = 0; iteration < kRepetitions; ++iteration) {
        g_received_strings.clear();

        const auto slots_ready = barrier_name("slots-ready", iteration);
        const auto messages_done = barrier_name("messages-done", iteration);
        const auto write_phase = barrier_name("write-phase", iteration);

        sintra::barrier(slots_ready);
        sintra::barrier(messages_done);

        write_strings(iteration_file(shared_dir, "strings", iteration), g_received_strings);

        sintra::barrier(write_phase);
    }

    return 0;
}

int process_int_receiver()
{
    auto int_slot = [](int value) {
        g_received_ints.push_back(value);
    };
    sintra::activate_slot(int_slot);

    const auto shared_dir = get_shared_directory();

    for (int iteration = 0; iteration < kRepetitions; ++iteration) {
        g_received_ints.clear();

        const auto slots_ready = barrier_name("slots-ready", iteration);
        const auto messages_done = barrier_name("messages-done", iteration);
        const auto write_phase = barrier_name("write-phase", iteration);

        sintra::barrier(slots_ready);
        sintra::barrier(messages_done);

        write_ints(iteration_file(shared_dir, "ints", iteration), g_received_ints);

        sintra::barrier(write_phase);
    }

    return 0;
}

} // namespace

void custom_terminate_handler() {
    std::fprintf(stderr, "std::terminate called!\n");
    std::fprintf(stderr, "Uncaught exceptions: %d\n", std::uncaught_exceptions());

    try {
        auto eptr = std::current_exception();
        if (eptr) {
            std::rethrow_exception(eptr);
        }
        else {
            std::fprintf(stderr, "terminate called without an active exception\n");
        }
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Uncaught exception: %s\n", e.what());
    }
    catch (...) {
        std::fprintf(stderr, "Uncaught exception of unknown type\n");
    }

    std::abort();
}

int main(int argc, char* argv[])
{
    std::set_terminate(custom_terminate_handler);

    const bool is_spawned = has_branch_flag(argc, argv);

    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(process_sender);
    processes.emplace_back(process_string_receiver);
    processes.emplace_back(process_int_receiver);

    sintra::init(argc, argv, processes);
    sintra::finalize();

    int result = 0;
    if (!is_spawned) {
        const auto result_path = shared_dir / "result.txt";
        if (!std::filesystem::exists(result_path)) {
            result = 1;
        }
        else {
            std::ifstream in(result_path, std::ios::binary);
            std::string status;
            in >> status;
            result = (status == "ok") ? 0 : 1;
        }

        try {
            std::filesystem::remove_all(shared_dir);
        }
        catch (const std::exception& e) {
            std::fprintf(stderr,
                         "Warning: failed to remove temp directory %s: %s\n",
                         shared_dir.string().c_str(),
                         e.what());
        }
    }

    clear_shared_directory_env();
    return result;
}

