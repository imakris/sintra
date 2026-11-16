// W3, swarm formation and shutdown: a fresh coordinator starts N leaf branches
// whose entry functions are empty, the swarm meets once, and every process
// calls `sintra::shutdown()`.
//
//   <exe> <cell options> --run-id <id>   runs one sample as a swarm
//   <exe> --aggregate <dir>              prints the sample's record as one JSON object
//   <exe> --build-facts                  prints the build facts
//
// benchmarks/README.md describes the options; docs/benchmark_plan.md defines the
// protocol, the metrics and the record.

#include "harness.h"

#include <sintra/sintra.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace bench = sintra::bench;

constexpr char k_formed[] = "benchmark-formed";

// A coordinator takes the first automatic process index and hands the others
// out to its branches.
constexpr uint64_t k_max_leaves = sintra::max_process_index - 2;

// Validation only: a branch that cannot be spawned, so that init throws.
constexpr char k_missing_binary[] = "sintra_swarm_benchmark_missing_binary";

// The cell of a sample. Every process parses it from the same options.
struct swarm_cell_t
{
    uint64_t leaves;
    uint64_t cpus;                  // C of the session; 0 when it is unknown
    uint64_t fail_init;
    uint64_t leaf_exit_delay_ms;

    bool operator==(const swarm_cell_t&) const = default;
};

// The workload block of a process report. The report header holds the shutdown
// snapshots: measure-begin after the rendezvous, before the first shutdown()
// call, and measure-end after the shutdown() call that returned true.
struct swarm_block_t
{
    swarm_cell_t cell;

    uint64_t main_entry_ns;
    uint64_t init_begin_ns;
    uint64_t ready_ns;                  // formation snapshot, right after init
    double   ready_cpu_s;               // cumulative process CPU at that snapshot
    uint64_t first_shutdown_end_ns;     // return of the first shutdown() call
    uint64_t shutdown_calls;
    uint64_t init_failed;
    uint64_t shutdown_threw;
};

using Report = bench::Process_report<swarm_block_t>;

const char* role_name(int branch_index)
{
    return branch_index == 0 ? "coordinator" : "leaves";
}

// A sample is validation-only when an option changed the effective workload.
bool is_validation_only(const swarm_cell_t& cell)
{
    return cell.fail_init != 0 || cell.leaf_exit_delay_ms != 0;
}


// ---------------------------------------------------------------------------
// Run mode
// ---------------------------------------------------------------------------

int leaf()
{
    return 0;
}

void parse_cell(int argc, char* argv[], Report& report)
{
    const auto count = [&](const char* flag, const std::string& fallback) {
        return bench::parse_count(bench::option_value(argc, argv, flag, fallback), flag);
    };
    const auto run_id = bench::option_value(argc, argv, "--run-id", "");
    if (run_id.empty() || run_id.size() >= sizeof(report.header.run_id)) {
        throw std::invalid_argument("--run-id must have 1..63 characters");
    }

    auto& cell = report.workload.cell;
    cell.leaves             = count("--leaves", "1");
    cell.cpus               = count("--cpus", std::to_string(std::thread::hardware_concurrency()));
    cell.fail_init          = bench::has_option(argc, argv, "--fail-init") ? 1 : 0;
    cell.leaf_exit_delay_ms = count("--leaf-exit-delay-ms", "0");
    if (cell.leaves == 0 || cell.leaves > k_max_leaves) {
        throw std::invalid_argument("--leaves must be 1.." + std::to_string(k_max_leaves));
    }
    run_id.copy(report.header.run_id, run_id.size());
}

int run(int argc, char* argv[], uint64_t main_entry_ns)
{
    Report report;
    parse_cell(argc, argv, report);
    auto&       process = report.workload;
    const auto& cell    = process.cell;
    process.main_entry_ns = main_entry_ns;

    // The library passes a branch its index on the command line. The index is
    // read from there because the report is written after the runtime is gone,
    // also when init failed. It is checked against the library's own below.
    const auto branch_index = (int)bench::parse_count(
        bench::option_value(argc, argv, "--branch_index", "0"), "--branch_index");
    const std::string_view role = role_name(branch_index);
    role.copy(report.header.role, role.size());
    report.header.branch_index = branch_index;

    const auto run_directory = bench::run_directory();
    bench::record_pid(run_directory);
    sintra::set_log_callback(&bench::log_callback, nullptr);

    // Each branch parses the cell from the coordinator's own options.
    std::vector<sintra::Process_descriptor> processes(
        (size_t)cell.leaves, sintra::Process_descriptor(leaf));
    if (cell.fail_init != 0) {
        processes.emplace_back(k_missing_binary);
    }
    const std::vector<std::string> options(argv + 1, argv + argc);
    for (auto& descriptor : processes) {
        descriptor.user_options = options;
    }

    // The coordinator's init returns after the startup barrier of all
    // processes. A branch's init runs the empty entry function before it
    // returns, so the formation snapshot follows formation in every process.
    const auto mark_ready = [&] {
        process.ready_ns    = bench::now_ns();
        process.ready_cpu_s = bench::cpu_seconds();
    };
    process.init_begin_ns = bench::now_ns();
    try {
        sintra::init(argc, argv, processes);
    }
    catch (...) {
        // The library tears down what init left behind when the process exits.
        mark_ready();
        process.init_failed = 1;
        bench::write_report(run_directory, report);
        throw;
    }
    mark_ready();
    if (sintra::process_index() != branch_index) {
        throw std::runtime_error("--branch_index is not the library's index of this process");
    }

    // Neither phase's CPU interval includes the rendezvous. If it was bypassed,
    // the shutdown snapshots would not follow a formed swarm, so the process
    // fails the sample: this throws, and the process writes no report.
    bench::rendezvous(k_formed);

    // A shutdown() that returns false has retained the runtime for a retry,
    // which resumes the teardown (docs/reference/shutdown.md). The shutdown
    // phase runs until the call that returns true, and the runner's timeout
    // bounds the retries.
    bench::mark_measure_begin(report.header);
    std::exception_ptr failure;
    try {
        bool torn_down = sintra::shutdown();
        process.first_shutdown_end_ns = bench::now_ns();
        process.shutdown_calls        = 1;
        while (!torn_down) {
            torn_down = sintra::shutdown();
            ++process.shutdown_calls;
        }
    }
    catch (...) {
        failure                = std::current_exception();
        process.shutdown_threw = 1;
    }
    bench::mark_measure_end(report.header);
    bench::write_report(run_directory, report);
    if (failure) {
        std::rethrow_exception(failure);
    }

    // Validation only: while a leaf outlives its shutdown(), the coordinator's
    // shutdown() returns false.
    if (branch_index != 0 && cell.leaf_exit_delay_ms != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(cell.leaf_exit_delay_ms));
    }
    return 0;
}


// ---------------------------------------------------------------------------
// Aggregate mode
// ---------------------------------------------------------------------------

double seconds(uint64_t begin_ns, uint64_t end_ns)
{
    return (double)(end_ns - begin_ns) / 1.0e9;
}

// From just before init to the formation snapshot.
double ready_s(const Report& process)
{
    return seconds(process.workload.init_begin_ns, process.workload.ready_ns);
}

// From the entry of main to the formation snapshot.
double start_s(const Report& process)
{
    return seconds(process.workload.main_entry_ns, process.workload.ready_ns);
}

// From the first shutdown() call to the return of the call that returned true.
double shutdown_s(const Report& process)
{
    return seconds(process.header.measure_begin_ns, process.header.measure_end_ns);
}

double first_shutdown_call_s(const Report& process)
{
    return seconds(process.header.measure_begin_ns, process.workload.first_shutdown_end_ns);
}

double shutdown_cpu_s(const Report& process)
{
    return process.header.cpu_end_s - process.header.cpu_begin_s;
}

void append_cell(bench::Json_writer& json, const swarm_cell_t& cell)
{
    json.begin_object("cell");
    json.text("workload", "swarm");
    json.text("mode", "lifecycle");
    json.null("origin");
    json.null("receivers");
    json.null("payload_bytes");
    json.null("framed_bytes");
    json.null("gap_us");
    json.null("window");
    json.count("n_leaves", cell.leaves);
    json.null("offered_rate");
    json.null("planned");
    json.null("warmup");
    json.null("burst");
    json.null("handler_delay_us");
    json.null("drain_timeout_s");
    json.flag("validation_only", is_validation_only(cell));
    json.end_object();
}

// Maps each role to the seconds that `process_cpu_s` yields for its processes.
template <typename Process_cpu>
void append_cpu_by_role(
    bench::Json_writer&         json,
    std::string_view            key,
    const std::vector<Report>&  processes,
    Process_cpu                 process_cpu_s)
{
    double coordinator = 0;
    double leaves      = 0;
    for (const auto& process : processes) {
        (process.header.branch_index == 0 ? coordinator : leaves) += process_cpu_s(process);
    }
    json.begin_object(key);
    json.number("coordinator", coordinator);
    json.number("leaves", leaves);
    json.number("total", coordinator + leaves);
    json.end_object();
}

void append_lifecycle(
    bench::Json_writer&         json,
    const swarm_cell_t&         cell,
    const Report&               coordinator,
    const std::vector<Report>&  processes)
{
    double leaf_start_max    = 0;
    double leaf_shutdown_max = 0;
    for (const auto& process : processes) {
        if (process.header.branch_index != 0) {
            leaf_start_max    = std::max(leaf_start_max, start_s(process));
            leaf_shutdown_max = std::max(leaf_shutdown_max, shutdown_s(process));
        }
    }

    json.begin_object("lifecycle");
    json.number("time_to_ready_s", ready_s(coordinator));
    json.number("time_to_shutdown_s", shutdown_s(coordinator));
    json.count("shutdown_calls", coordinator.workload.shutdown_calls);
    json.number("first_shutdown_call_s", first_shutdown_call_s(coordinator));
    json.number("leaf_start_max_s", leaf_start_max);
    json.number("leaf_shutdown_max_s", leaf_shutdown_max);
    append_cpu_by_role(json, "formation_cpu", processes,
        [](const Report& process) { return process.workload.ready_cpu_s; });
    append_cpu_by_role(json, "shutdown_cpu", processes, shutdown_cpu_s);
    json.number(
        "oversubscription",
        cell.cpus != 0
            ? std::optional<double>((double)(cell.leaves + 1) / (double)cell.cpus)
            : std::nullopt);
    json.end_object();
}

int aggregate(const std::filesystem::path& directory)
{
    const auto set = bench::read_report_set<swarm_block_t>(
        directory,
        [](const swarm_cell_t& cell) { return 1 + (int)cell.leaves; },
        [](const swarm_cell_t&, int branch_index) { return role_name(branch_index); });
    const auto& processes = set.processes;

    auto                     reasons = bench::build_invalid_reasons();
    std::vector<std::string> warnings;
    reasons.insert(reasons.end(), set.reasons.begin(), set.reasons.end());
    const auto any_process = [&](auto&& predicate) {
        return std::any_of(processes.begin(), processes.end(), predicate);
    };
    const bool init_failed = any_process(
        [](const Report& process) { return process.workload.init_failed != 0; });
    const bool shutdown_threw = any_process(
        [](const Report& process) { return process.workload.shutdown_threw != 0; });
    if (init_failed) {
        reasons.push_back("init_failed");
    }
    if (shutdown_threw) {
        reasons.push_back("shutdown_threw");
    }

    bench::Json_writer json;
    json.begin_object();
    json.text("schema", "sintra-bench/1");
    json.text("run_id", set.run_id);
    bench::append_build_facts(json, "build");
    append_cell(json, set.cell);
    json.null("counts");
    json.null("throughput");
    json.null("latency");
    json.null("cpu");
    // The lifecycle metrics need both completed phases of every process.
    if (set.complete && !init_failed && !shutdown_threw) {
        const auto& coordinator = *std::find_if(processes.begin(), processes.end(),
            [](const Report& process) { return process.header.branch_index == 0; });
        // A process CPU clock that did not move over a phase says nothing
        // about the role's cost.
        if (any_process([](const Report& process) {
                return process.workload.ready_cpu_s == 0 || shutdown_cpu_s(process) == 0;
            }))
        {
            warnings.push_back("cpu_not_advancing");
        }
        // A shutdown() that returned false and was retried is a result, not an
        // invalidity.
        if (any_process([](const Report& process) { return process.workload.shutdown_calls > 1; }))
        {
            warnings.push_back("shutdown_retried");
        }
        append_lifecycle(json, set.cell, coordinator, processes);
    }
    else {
        json.null("lifecycle");
    }
    json.begin_object("outcome");
    bench::append_names(json, "missing_files", set.missing);
    json.flag("valid", reasons.empty());
    bench::append_names(json, "reasons", reasons);
    bench::append_names(json, "warnings", warnings);
    json.end_object();
    json.end_object();
    std::printf("%s\n", json.str().c_str());
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    // A leaf's start-up time runs from here to its formation snapshot.
    const auto main_entry_ns = bench::now_ns();
    try {
        if (bench::has_option(argc, argv, "--build-facts")) {
            bench::Json_writer json;
            bench::append_build_facts(json);
            std::printf("%s\n", json.str().c_str());
            return 0;
        }
        if (bench::has_option(argc, argv, "--aggregate")) {
            return aggregate(bench::option_value(argc, argv, "--aggregate"));
        }
        return run(argc, argv, main_entry_ns);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "swarm_benchmark: %s\n", error.what());
        return 1;
    }
}
