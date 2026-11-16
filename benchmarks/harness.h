// Shared harness of the Sintra benchmarks: the clock and process CPU time, build
// facts, per-process report files, and the aggregation and JSON helpers.
//
// A benchmark process keeps its raw data in memory while it measures and then
// writes one report file. The same executable reads the files back in its
// --aggregate mode.

#pragma once

#include <sintra/sintra.h>

#ifdef _WIN32
#include <sintra/detail/sintra_windows.h>
#else
#include <sys/resource.h>
#endif

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef SINTRA_BENCHMARK_CONFIG
#define SINTRA_BENCHMARK_CONFIG ""
#endif

namespace sintra::bench {

using Clock = std::chrono::steady_clock;


// ---------------------------------------------------------------------------
// Clock and process CPU time
// ---------------------------------------------------------------------------

inline uint64_t now_ns()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

/// User plus system time of every thread of this process. This is the only
/// OS-specific code in the benchmarks; the PID that a process records comes from
/// the library.
inline double cpu_seconds()
{
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        throw std::runtime_error("GetProcessTimes failed");
    }
    const auto ticks = [](FILETIME value) {
        return ((uint64_t)value.dwHighDateTime << 32) | value.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) / 1.0e7;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        throw std::runtime_error("getrusage failed");
    }
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
        (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1.0e6;
#endif
}


// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

inline bool has_option(int argc, char* argv[], std::string_view flag)
{
    for (int i = 1; i < argc; ++i) {
        if (flag == argv[i]) {
            return true;
        }
    }
    return false;
}

/// The argument that follows `flag`, or `fallback` when the flag is absent.
inline std::string option_value(
    int argc, char* argv[], std::string_view flag, std::string_view fallback = {})
{
    for (int i = 1; i < argc; ++i) {
        if (flag == argv[i]) {
            if (i + 1 == argc) {
                throw std::invalid_argument(std::string(flag) + " needs a value");
            }
            return argv[i + 1];
        }
    }
    return std::string(fallback);
}

/// A non-negative integer. The whole text must be the number, so "9.5" and
/// "9x" are rejected, not truncated.
inline uint64_t parse_count(const std::string& text, std::string_view flag)
{
    uint64_t   value = 0;
    const auto end   = text.data() + text.size();
    const auto parse = std::from_chars(text.data(), end, value);
    if (parse.ec != std::errc() || parse.ptr != end) {
        throw std::invalid_argument(
            std::string(flag) + " needs a non-negative integer, not '" + text + "'");
    }
    return value;
}

/// A finite, positive number. The whole text must be the number.
inline double parse_positive_number(const std::string& text, std::string_view flag)
{
    char*        end   = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value) ||
        !(value > 0))
    {
        throw std::invalid_argument(
            std::string(flag) + " needs a finite, positive number, not '" + text + "'");
    }
    return value;
}


// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

inline uint64_t splitmix64(uint64_t value)
{
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

/// Keeps about 100000 samples of a run of `planned` messages.
inline uint64_t sampling_divisor(uint64_t planned)
{
    return std::max<uint64_t>(1, (planned + 99999) / 100000);
}

/// Every process and both arms of a pair sample the same sequences, so the
/// samples of different processes can be joined on the sequence.
inline bool is_sampled(uint64_t sequence, uint64_t divisor)
{
    return splitmix64(sequence) % divisor == 0;
}


// ---------------------------------------------------------------------------
// Library observations
// ---------------------------------------------------------------------------

inline std::atomic<uint64_t> eviction_log_matches{0};

/// Counts the eviction warnings of this process. The count corroborates the
/// sequence gaps and is never used alone.
inline void log_callback(sintra::log_level level, const char* message, void*)
{
    if (std::string_view(message).find("reader evicted on") != std::string_view::npos) {
        eviction_log_matches.fetch_add(1);
    }
    if (level == sintra::log_level::error) {
        std::fprintf(stderr, "%s\n", message);
    }
}

/// Size of the frame that the calling handler thread is dispatching. This is one
/// of the benchmarks' two couplings to library internals. The other is the
/// `--branch_index` argument of a spawned branch, which swarm_benchmark.cpp
/// reads.
inline uint64_t current_frame_bytes()
{
    return sintra::s_tl_current_message->bytes_to_next_message;
}


// ---------------------------------------------------------------------------
// JSON output
// ---------------------------------------------------------------------------

/// Builds one JSON value as text. An empty key writes an array element or the
/// root value. Absent optionals and non-finite numbers are written as null.
class Json_writer
{
public:
    void begin_object(std::string_view key = {}) { open(key, '{'); }
    void end_object()                            { close('}'); }
    void begin_array(std::string_view key)       { open(key, '['); }
    void end_array()                             { close(']'); }

    void null(std::string_view key)
    {
        item(key);
        m_text += "null";
    }

    void flag(std::string_view key, bool value)
    {
        item(key);
        m_text += value ? "true" : "false";
    }

    void count(std::string_view key, std::optional<uint64_t> value)
    {
        item(key);
        m_text += value ? std::to_string(*value) : "null";
    }

    void number(std::string_view key, std::optional<double> value)
    {
        item(key);
        if (!value || !std::isfinite(*value)) {
            m_text += "null";
            return;
        }
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.12g", *value);
        m_text += buffer;
    }

    void text(std::string_view key, std::string_view value)
    {
        item(key);
        quote(value);
    }

    const std::string& str() const { return m_text; }

private:
    void item(std::string_view key)
    {
        if (m_needs_comma) {
            m_text += ',';
        }
        m_needs_comma = true;
        if (!key.empty()) {
            quote(key);
            m_text += ':';
        }
    }

    void open(std::string_view key, char bracket)
    {
        item(key);
        m_text += bracket;
        m_needs_comma = false;
    }

    void close(char bracket)
    {
        m_text += bracket;
        m_needs_comma = true;
    }

    void quote(std::string_view value)
    {
        m_text += '"';
        for (const char c : value) {
            if (c == '"' || c == '\\') {
                m_text += '\\';
                m_text += c;
            }
            else
            if ((unsigned char)c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", (unsigned)c);
                m_text += buffer;
            }
            else {
                m_text += c;
            }
        }
        m_text += '"';
    }

    std::string m_text;
    bool        m_needs_comma = false;
};

/// Writes an array of strings, such as the reasons or the warnings of a record.
inline void append_names(
    Json_writer& json, std::string_view key, const std::vector<std::string>& names)
{
    json.begin_array(key);
    for (const auto& name : names) {
        json.text({}, name);
    }
    json.end_array();
}


// ---------------------------------------------------------------------------
// Build facts
// ---------------------------------------------------------------------------

inline constexpr std::string_view k_build_config = SINTRA_BENCHMARK_CONFIG;

#ifdef NDEBUG
inline constexpr bool k_build_ndebug = true;
#else
inline constexpr bool k_build_ndebug = false;
#endif

// MSVC has no optimization macro: a Release configuration without _DEBUG
// stands for it.
#if defined(__OPTIMIZE__)
inline constexpr bool k_build_optimized = true;
#elif defined(_MSC_VER) && !defined(_DEBUG)
inline constexpr bool k_build_optimized = k_build_config == "Release";
#else
inline constexpr bool k_build_optimized = false;
#endif

#if defined(SINTRA_ENABLE_TEST_HOOKS)
inline constexpr bool k_build_test_hooks = true;
#else
inline constexpr bool k_build_test_hooks = false;
#endif

#if defined(SINTRA_SEPARATE_COMPILATION)
inline constexpr bool k_build_separate_compilation = true;
#else
inline constexpr bool k_build_separate_compilation = false;
#endif

inline std::string build_compiler()
{
#if defined(__clang__)
    return __VERSION__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#elif defined(_MSC_FULL_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown";
#endif
}

/// The reasons why a sample of this build cannot be compared. Empty for a
/// Release configuration with NDEBUG, optimization on and test hooks off.
inline std::vector<std::string> build_invalid_reasons()
{
    std::vector<std::string> reasons;
    if (k_build_config != "Release") { reasons.push_back("build_not_release"); }
    if (!k_build_ndebug)             { reasons.push_back("build_without_ndebug"); }
    if (!k_build_optimized)          { reasons.push_back("build_not_optimized"); }
    if (k_build_test_hooks)          { reasons.push_back("build_with_test_hooks"); }
    return reasons;
}

inline void append_build_facts(Json_writer& json, std::string_view key = {})
{
    json.begin_object(key);
    json.text("config", k_build_config);
    json.flag("ndebug", k_build_ndebug);
    json.flag("optimized", k_build_optimized);
    json.text("compiler", build_compiler());
    json.flag("test_hooks", k_build_test_hooks);
    json.flag("separate_compilation", k_build_separate_compilation);
    json.flag("slow_reader_eviction", SINTRA_ENABLE_SLOW_READER_EVICTION != 0);
#if defined(SINTRA_EVICTION_SPIN_THRESHOLD) && SINTRA_EVICTION_SPIN_THRESHOLD > 0
    json.null("eviction_spin_budget_us");
    json.count("eviction_spin_threshold", (uint64_t)SINTRA_EVICTION_SPIN_THRESHOLD);
#else
    json.count("eviction_spin_budget_us", (uint64_t)SINTRA_EVICTION_SPIN_BUDGET_US);
    json.null("eviction_spin_threshold");
#endif
    json.count("eviction_lag_rings", (uint64_t)SINTRA_EVICTION_LAG_RINGS);
    json.count("message_ring_size", (uint64_t)sintra::message_ring_size);
    json.count("ring_abi_version", sintra::detail::k_sintra_ring_abi_version);
    json.text("ring_parent_dir", std::filesystem::temp_directory_path().string());
    json.count("hardware_concurrency", (uint64_t)std::thread::hardware_concurrency());
    json.flag("valid_build", build_invalid_reasons().empty());
    json.end_object();
}


// ---------------------------------------------------------------------------
// Per-process report files
// ---------------------------------------------------------------------------

inline constexpr uint64_t k_report_magic   = 0x48434e4542544e53ull; // "SNTBENCH"
inline constexpr uint32_t k_report_version = 1;

struct sample_t
{
    uint64_t key;       // message or call sequence, or the index of an interval
    uint64_t begin_ns;
    uint64_t end_ns;
};

struct report_header_t
{
    uint64_t magic;
    uint32_t version;
    uint32_t workload_bytes;
    char     run_id[64];
    char     role[32];
    int32_t  branch_index;
    uint32_t reserved;
    double   cpu_begin_s;
    double   cpu_end_s;
    uint64_t measure_begin_ns;
    uint64_t measure_end_ns;
    uint64_t eviction_log_matches;
    uint64_t sample_count;
    uint64_t interval_count;
};

/// One process's raw data. `Workload` is the executable's own fixed-size block:
/// the cell and the counters of the process's role.
template <typename Workload>
struct Process_report
{
    report_header_t        header{};
    Workload               workload{};
    std::vector<sample_t>  samples;     // sampled messages or calls
    std::vector<sample_t>  intervals;   // other timed intervals, such as fences
};

/// Begin snapshot, common to every process and role: taken after the warm-up
/// fence returns and before the begin rendezvous, so it precedes the first
/// measured message or call. The eviction-log count restarts here.
inline void mark_measure_begin(report_header_t& header)
{
    eviction_log_matches    = 0;
    header.cpu_begin_s      = cpu_seconds();
    header.measure_begin_ns = now_ns();
}

/// End snapshot, common to every process and role: taken right after the
/// mode's end barrier returns and before the last rendezvous.
inline void mark_measure_end(report_header_t& header)
{
    header.measure_end_ns = now_ns();
    header.cpu_end_s      = cpu_seconds();
}

/// The run directory that the runner names in SINTRA_BENCHMARK_DIR.
inline std::filesystem::path run_directory()
{
    const char* value = std::getenv("SINTRA_BENCHMARK_DIR");
    if (!value || !*value) {
        throw std::runtime_error("SINTRA_BENCHMARK_DIR must name an existing run directory");
    }
    return value;
}

/// Records this process's PID in the run directory at start-up. The runner
/// counts the participants by these files, so that it knows how many reports
/// to wait for. It does not find or signal a process through them.
inline void record_pid(const std::filesystem::path& directory)
{
    const auto file = directory / (std::to_string(sintra::get_current_pid()) + ".pid");
    if (!std::ofstream(file)) {
        throw std::runtime_error("cannot write " + file.string());
    }
}

inline std::string report_file_name(std::string_view role, int branch_index)
{
    return std::string(role) + '-' + std::to_string(branch_index) + ".bin";
}

/// Writes `<role>-<branch index>.bin`. The file appears under its final name
/// only when it is complete.
template <typename Workload>
void write_report(
    const std::filesystem::path& directory, const Process_report<Workload>& report)
{
    static_assert(std::is_trivially_copyable_v<Workload>);
    auto header           = report.header;
    header.magic          = k_report_magic;
    header.version        = k_report_version;
    header.workload_bytes = (uint32_t)sizeof(Workload);
    header.sample_count   = report.samples.size();
    header.interval_count = report.intervals.size();

    const auto file    = directory / report_file_name(header.role, header.branch_index);
    const auto partial = std::filesystem::path(file.string() + ".tmp");
    const auto bytes   = [](const std::vector<sample_t>& values) {
        return (std::streamsize)(values.size() * sizeof(sample_t));
    };
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    output.write((const char*)&header, sizeof(header));
    output.write((const char*)&report.workload, sizeof(Workload));
    output.write((const char*)report.samples.data(), bytes(report.samples));
    output.write((const char*)report.intervals.data(), bytes(report.intervals));
    output.close();
    if (!output) {
        throw std::runtime_error("cannot write " + partial.string());
    }
    std::filesystem::rename(partial, file);
}

template <typename Workload>
Process_report<Workload> read_report(const std::filesystem::path& file)
{
    Process_report<Workload> report;
    auto&                    header = report.header;
    std::ifstream            input(file, std::ios::binary);
    input.read((char*)&header, sizeof(header));
    if (!input || header.magic != k_report_magic || header.version != k_report_version ||
        header.workload_bytes != sizeof(Workload))
    {
        throw std::runtime_error("not a report of this executable: " + file.string());
    }
    // The strings must be terminated, and the counts must account for every
    // byte of the file, so that a damaged count cannot request an absurd
    // allocation.
    const auto terminated = [](const auto& text) {
        return std::memchr(text, '\0', sizeof(text)) != nullptr;
    };
    const auto fixed_bytes = sizeof(header) + sizeof(Workload);
    const auto file_bytes  = std::filesystem::file_size(file);
    if (!terminated(header.run_id) || !terminated(header.role) || file_bytes < fixed_bytes ||
        (file_bytes - fixed_bytes) % sizeof(sample_t) != 0 ||
        header.sample_count > (file_bytes - fixed_bytes) / sizeof(sample_t) ||
        header.interval_count !=
            (file_bytes - fixed_bytes) / sizeof(sample_t) - header.sample_count)
    {
        throw std::runtime_error("damaged report: " + file.string());
    }
    report.samples.resize((size_t)header.sample_count);
    report.intervals.resize((size_t)header.interval_count);
    input.read((char*)&report.workload, sizeof(Workload));
    input.read(
        (char*)report.samples.data(),
        (std::streamsize)(report.samples.size() * sizeof(sample_t)));
    input.read(
        (char*)report.intervals.data(),
        (std::streamsize)(report.intervals.size() * sizeof(sample_t)));
    if (!input) {
        throw std::runtime_error("truncated report: " + file.string());
    }
    return report;
}

/// Reads every report of a run directory, ordered by file name.
template <typename Workload>
std::vector<Process_report<Workload>> read_reports(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".bin") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::vector<Process_report<Workload>> reports;
    for (const auto& file : files) {
        reports.push_back(read_report<Workload>(file));
    }
    return reports;
}

/// The reports of one run directory, checked against the cell that they
/// describe: the result of read_report_set().
template <typename Workload>
struct Report_set
{
    std::vector<Process_report<Workload>>  processes;   // those of the run and the cell
    std::string                            run_id;
    decltype(Workload::cell)               cell{};
    std::vector<std::string>               missing;     // file names of the absent reports
    std::vector<std::string>               reasons;     // why the set is not the expected one
    bool                                   complete = false;
};

/// Reads the reports of a run directory and checks that they are the ones that
/// their cell expects. Every benchmark executable aggregates from this.
///
/// The coordinator's report (branch index 0), or else the first report, names
/// the run and the cell. A report of another run or of another cell is dropped,
/// so the process it stands for counts as missing. The cell expects exactly one
/// report per process, for the branch indexes from 0 on, each under the role
/// of its branch. `complete` says that this holds, so that the sample's metrics
/// can be computed. `reasons` holds what makes the sample invalid: any of
/// missing_files, foreign_run_files, inconsistent_reports, unexpected_reports.
///
/// An executable supplies:
/// - `Workload`, its block of a process report, whose member `cell` compares
///   with ==;
/// - `process_count(cell)`, the number of processes of the cell's swarm, the
///   coordinator included, as an int;
/// - `role_of(cell, branch_index)`, the role name that the process with that
///   branch index writes into its report header.
///
/// Throws when the directory holds no report.
template <typename Workload, typename Process_count, typename Role_of>
Report_set<Workload> read_report_set(
    const std::filesystem::path&  directory,
    Process_count                 process_count,
    Role_of                       role_of)
{
    Report_set<Workload> set;
    auto&                processes = set.processes;
    processes = read_reports<Workload>(directory);
    if (processes.empty()) {
        throw std::runtime_error("no process reports in " + directory.string());
    }

    const auto coordinator = std::find_if(processes.begin(), processes.end(),
        [](const auto& process) { return process.header.branch_index == 0; });
    const auto& reference = coordinator != processes.end() ? *coordinator : processes.front();
    set.run_id = reference.header.run_id;
    set.cell   = reference.workload.cell;
    const auto foreign = std::erase_if(processes,
        [&](const auto& process) { return process.header.run_id != set.run_id; });
    const auto inconsistent = std::erase_if(processes,
        [&](const auto& process) { return !(process.workload.cell == set.cell); });

    // Whatever remains beyond the expected reports is a duplicate or belongs to
    // a branch that the cell lacks.
    const int expected = process_count(set.cell);
    for (int branch_index = 0; branch_index < expected; ++branch_index) {
        const std::string_view role  = role_of(set.cell, branch_index);
        const bool             found = std::any_of(processes.begin(), processes.end(),
            [&](const auto& process) {
                return process.header.branch_index == branch_index &&
                    role == process.header.role;
            });
        if (!found) {
            set.missing.push_back(report_file_name(role, branch_index));
        }
    }
    const bool unexpected = processes.size() != (size_t)expected - set.missing.size();
    set.complete = set.missing.empty() && !unexpected;

    const auto invalid_if = [&](bool condition, const char* reason) {
        if (condition) {
            set.reasons.push_back(reason);
        }
    };
    invalid_if(!set.missing.empty(), "missing_files");
    invalid_if(foreign != 0, "foreign_run_files");
    invalid_if(inconsistent != 0, "inconsistent_reports");
    invalid_if(unexpected, "unexpected_reports");
    return set;
}


// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------

/// Nearest rank over sorted nanosecond values, x[ceil(q*n)-1], in microseconds.
/// The quantile is given per mille so that the rank is computed exactly.
inline double percentile_us(const std::vector<uint64_t>& sorted_ns, uint64_t per_mille)
{
    const auto rank = (per_mille * sorted_ns.size() + 999) / 1000;
    return sorted_ns[(size_t)std::max<uint64_t>(rank, 1) - 1] / 1000.0;
}

/// Writes one latency object: {n_obs, n_messages, p50_us, p99_us, p999_us,
/// max_us}. The nearest rank is taken over the object's own observations,
/// `values_ns`. `messages` is the number of distinct messages, calls or bursts
/// behind them; an object that pools deliveries has more observations than
/// messages. p99.9 and the maximum are reported only from 10000 messages on.
inline void append_latency(
    Json_writer&            json,
    std::string_view        name,
    std::vector<uint64_t>&  values_ns,
    uint64_t                messages)
{
    std::sort(values_ns.begin(), values_ns.end());
    const bool any  = !values_ns.empty();
    const bool tail = any && messages >= 10000;
    const auto at   = [&](uint64_t per_mille) { return percentile_us(values_ns, per_mille); };

    json.begin_object(name);
    json.count("n_obs", values_ns.size());
    json.count("n_messages", messages);
    json.number("p50_us",  any  ? std::optional<double>(at(500))  : std::nullopt);
    json.number("p99_us",  any  ? std::optional<double>(at(990))  : std::nullopt);
    json.number("p999_us", tail ? std::optional<double>(at(999))  : std::nullopt);
    json.number("max_us",  tail ? std::optional<double>(at(1000)) : std::nullopt);
    json.end_object();
}

/// Process CPU of one role over the measured interval. A process with two
/// roles is reported once, under a combined role name.
struct Role_cpu
{
    std::string  role;
    uint64_t     processes         = 0;
    double       cpu_s             = 0;
    double       max_process_cpu_s = 0;
};

inline void add_process_cpu(Role_cpu& role, double process_cpu_s)
{
    ++role.processes;
    role.cpu_s            += process_cpu_s;
    role.max_process_cpu_s = std::max(role.max_process_cpu_s, process_cpu_s);
}

/// Writes one CPU object: {processes, cpu_s, max_process_cpu_s, us_per_message,
/// us_per_delivery}. Only receiving roles pass `deliveries`.
inline void append_role_cpu(
    Json_writer&             json,
    const Role_cpu&          role,
    uint64_t                 messages,
    std::optional<uint64_t>  deliveries = std::nullopt)
{
    json.begin_object(role.role);
    json.count("processes", role.processes);
    json.number("cpu_s", role.cpu_s);
    json.number("max_process_cpu_s", role.max_process_cpu_s);
    const double cpu_us = role.cpu_s * 1.0e6;
    json.number("us_per_message", cpu_us / (double)messages);
    json.number(
        "us_per_delivery",
        deliveries ? std::optional<double>(cpu_us / (double)*deliveries) : std::nullopt);
    json.end_object();
}

} // namespace sintra::bench
