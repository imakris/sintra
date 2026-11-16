// W2, request/response: one caller thread in a leaf calls `echo` on a callee
// transceiver that lives in the coordinator or in another leaf. A closed-loop
// cell makes one synchronous call at a time, with an idle gap before each; the
// windowed cell keeps a window of asynchronous calls outstanding.
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

using bench::has_role;
using bench::invalid_if;
using bench::processing_barrier;
using bench::rendezvous;
using bench::run_protocol;

constexpr char     k_callee_name[] = "benchmark-callee";
constexpr uint64_t k_warmup_value  = UINT64_MAX;

// Windowed mode: a call whose result is not observed within this time of its
// issue counts as lost.
constexpr auto k_call_timeout = std::chrono::seconds(10);

enum class Mode : uint32_t
{
    CLOSED_LOOP,
    WINDOWED,
};

enum class Route : uint32_t
{
    LEAF_TO_COORDINATOR,
    LEAF_TO_LEAF,
};

// The cell of a sample. Every process parses it from the same options.
struct rpc_cell_t
{
    Mode     mode;
    Route    route;
    uint64_t planned;
    uint64_t warmup;
    uint64_t gap_us;            // closed-loop mode; 0 in windowed mode
    uint64_t window;            // windowed mode; 0 in closed-loop mode
    uint64_t handler_delay_us;
    uint64_t wrong_echo_every;
    uint64_t throw_every;

    bool operator==(const rpc_cell_t&) const = default;
};

// The workload block of a process report: the cell, then the caller's
// counters. The caller's samples hold every call that returned a value: its
// sequence, the start of the call and its return or observation.
struct rpc_block_t
{
    rpc_cell_t cell;

    uint64_t issued;
    uint64_t calls_begin_ns;
    uint64_t timed_out;
    uint64_t exceptions;
    uint64_t wrong_echoes;
};

using Report = bench::Process_report<rpc_block_t>;

// This process's raw data and the run directory it is written to. Only the
// caller thread writes the counters and the samples.
Report                report;
std::filesystem::path run_directory;

const char* mode_name(Mode mode)
{
    return mode == Mode::CLOSED_LOOP ? "closed_loop" : "windowed";
}

const char* route_name(Route route)
{
    return route == Route::LEAF_TO_COORDINATOR ? "leaf_to_coordinator" : "leaf_to_leaf";
}

// The caller is the last leaf. In the leaf-to-leaf route, leaf 1 hosts the
// callee.
const char* role_name(Route route, int branch_index)
{
    if (branch_index == 0) {
        return route == Route::LEAF_TO_COORDINATOR ? "coordinator_callee" : "coordinator";
    }
    return route == Route::LEAF_TO_LEAF && branch_index == 1 ? "callee" : "caller";
}

unsigned leaf_count(const rpc_cell_t& cell)
{
    return cell.route == Route::LEAF_TO_LEAF ? 2 : 1;
}

// A sample is validation-only when an option changed the effective workload:
// a handler delay, or a callee that fails or answers wrongly.
bool is_validation_only(const rpc_cell_t& cell)
{
    return cell.handler_delay_us != 0 || cell.wrong_echo_every != 0 || cell.throw_every != 0;
}

// Whether the measured call with this sequence is one of every `every` calls.
bool is_nth_call(uint64_t sequence, uint64_t every)
{
    return every != 0 && (sequence + 1) % every == 0;
}

struct Callee : sintra::Derived_transceiver<Callee>
{
    // Returns its argument. The validation-only options apply to measured
    // calls: a busy wait, which takes the same time on every platform, then a
    // failure or a wrong result for every n-th call.
    uint64_t echo(uint64_t value)
    {
        const auto& cell = report.workload.cell;
        if (value != k_warmup_value) {
            if (cell.handler_delay_us != 0) {
                const auto resume = bench::now_ns() + cell.handler_delay_us * 1000;
                while (bench::now_ns() < resume) {
                }
            }
            if (is_nth_call(value, cell.throw_every)) {
                throw std::runtime_error("deliberate failure of a benchmark call");
            }
            if (is_nth_call(value, cell.wrong_echo_every)) {
                return value + 1;
            }
        }
        return value;
    }

    SINTRA_RPC_STRICT(echo)
};


// ---------------------------------------------------------------------------
// Run mode
// ---------------------------------------------------------------------------

// Called after the warm-up fence. Every process resets its eviction-log count
// and takes its begin snapshot before the rendezvous, and the caller makes its
// first measured call only after the rendezvous returns.
void begin_measurement()
{
    bench::mark_measure_begin(report.header);
    rendezvous("benchmark-begin");
}

// Called by the caller after its last call, and by every other process right
// after the begin rendezvous. The mode's end barrier is a rendezvous in
// closed-loop mode and the final fence in windowed mode. No process writes its
// report or shuts down before the last rendezvous returns.
void end_measurement_and_report()
{
    const auto& cell = report.workload.cell;
    if (cell.mode == Mode::CLOSED_LOOP) {
        rendezvous("benchmark-complete");
    }
    else {
        processing_barrier("benchmark-complete");
    }
    bench::mark_measure_end(report.header);
    rendezvous("benchmark-end");

    const int              branch_index = sintra::process_index();
    const std::string_view role         = role_name(cell.route, branch_index);
    role.copy(report.header.role, role.size());
    report.header.branch_index         = branch_index;
    report.header.eviction_log_matches = bench::eviction_log_matches.load();
    bench::write_report(run_directory, report);
}

// `end_ns` is the stamp that the caller took as soon as the call returned or
// its result was observed.
void record_result(uint64_t sequence, uint64_t begin_ns, uint64_t end_ns, uint64_t value)
{
    report.samples.push_back({sequence, begin_ns, end_ns});
    if (value != sequence) {
        ++report.workload.wrong_echoes;
    }
}

// One synchronous call at a time. The idle gap precedes every call, the first
// one included, so that every call meets the wait regime of its gap.
void call_closed_loop(sintra::instance_id_type callee)
{
    auto&       counts = report.workload;
    const auto& cell   = counts.cell;
    const auto  gap    = std::chrono::microseconds(cell.gap_us);
    for (uint64_t i = 0; i < cell.planned; ++i) {
        if (cell.gap_us != 0) {
            std::this_thread::sleep_for(gap);
        }
        const auto called = bench::now_ns();
        try {
            const auto value    = Callee::rpc_echo(callee, i);
            const auto returned = bench::now_ns();
            record_result(i, called, returned, value);
        }
        catch (const std::exception&) {
            ++counts.exceptions;
        }
    }
}

struct pending_call_t
{
    uint64_t                     sequence;
    uint64_t                     issued_ns;
    sintra::Rpc_handle<uint64_t> handle;
};

// Waits for the result of an outstanding call until 10 s after its issue. A
// call that is still pending then counts as lost.
void observe(pending_call_t& call)
{
    auto&      counts = report.workload;
    const auto issued = bench::Clock::time_point(
        std::chrono::duration_cast<bench::Clock::duration>(
            std::chrono::nanoseconds(call.issued_ns)));
    try {
        const auto value    = call.handle.get_until(issued + k_call_timeout);
        const auto observed = bench::now_ns();
        record_result(call.sequence, call.issued_ns, observed, value);
    }
    catch (const sintra::rpc_timeout&) {
        ++counts.timed_out;
    }
    catch (const std::exception&) {
        ++counts.exceptions;
    }
}

// Keeps a window of asynchronous calls outstanding and waits on the oldest
// one when the window is full.
void call_windowed(sintra::instance_id_type callee)
{
    auto&                       counts = report.workload;
    const auto&                 cell   = counts.cell;
    std::vector<pending_call_t> window((size_t)cell.window);
    size_t                      oldest      = 0;
    size_t                      outstanding = 0;
    const auto observe_oldest = [&] {
        observe(window[oldest]);
        oldest = (oldest + 1) % window.size();
        --outstanding;
    };

    for (uint64_t i = 0; i < cell.planned; ++i) {
        if (outstanding == window.size()) {
            observe_oldest();
        }
        auto& call     = window[(oldest + outstanding) % window.size()];
        call.sequence  = i;
        call.issued_ns = bench::now_ns();
        try {
            call.handle = Callee::rpc_async_echo(callee, i);
            ++outstanding;
        }
        catch (const std::exception&) {
            ++counts.exceptions;
        }
    }
    while (outstanding != 0) {
        observe_oldest();
    }
}

void run_caller()
{
    auto&       counts = report.workload;
    const auto& cell   = counts.cell;
    report.samples.reserve((size_t)cell.planned);

    // The callee was named before it entered the ready fence. Its name is
    // resolved once: a name argument would be resolved again on every call.
    processing_barrier("benchmark-ready");
    const sintra::instance_id_type callee = sintra::Resolvable_instance_id(k_callee_name);
    if (callee == sintra::invalid_instance_id) {
        throw std::runtime_error("the callee's name did not resolve");
    }
    for (uint64_t i = 0; i < cell.warmup; ++i) {
        Callee::rpc_echo(callee, k_warmup_value);
    }
    processing_barrier("benchmark-warm");
    begin_measurement();
    counts.calls_begin_ns = bench::now_ns();
    if (cell.mode == Mode::CLOSED_LOOP) {
        call_closed_loop(callee);
    }
    else {
        call_windowed(callee);
    }
    counts.issued = cell.planned;
    end_measurement_and_report();
}

// The control-thread protocol of a process that does not call: the
// coordinator, and the callee's leaf in the leaf-to-leaf route. The callee
// lives until the last rendezvous has returned.
void follow_caller(bool hosts_callee)
{
    std::optional<Callee> callee;
    if (hosts_callee) {
        callee.emplace();
        if (!callee->assign_name(k_callee_name)) {
            throw std::runtime_error("the callee could not be named");
        }
    }
    processing_barrier("benchmark-ready");
    processing_barrier("benchmark-warm");
    begin_measurement();
    end_measurement_and_report();
}

int caller_leaf()
{
    run_protocol(run_caller);
    return 0;
}

int callee_leaf()
{
    run_protocol([] { follow_caller(true); });
    return 0;
}

void parse_cell(int argc, char* argv[])
{
    const auto text = [&](const char* flag, const char* fallback) {
        return bench::option_value(argc, argv, flag, fallback);
    };
    const auto count = [&](const char* flag, const char* fallback) {
        return bench::parse_count(bench::option_value(argc, argv, flag, fallback), flag);
    };
    const auto mode   = text("--mode", "closed_loop");
    const auto route  = text("--route", "leaf_to_coordinator");
    const auto run_id = text("--run-id", "");
    if (mode != "closed_loop" && mode != "windowed") {
        throw std::invalid_argument("--mode must be closed_loop or windowed");
    }
    if (route != "leaf_to_coordinator" && route != "leaf_to_leaf") {
        throw std::invalid_argument("--route must be leaf_to_coordinator or leaf_to_leaf");
    }
    if (run_id.empty() || run_id.size() >= sizeof(report.header.run_id)) {
        throw std::invalid_argument("--run-id must have 1..63 characters");
    }

    auto&      cell     = report.workload.cell;
    const bool windowed = mode == "windowed";
    if (bench::has_option(argc, argv, windowed ? "--gap-us" : "--window")) {
        throw std::invalid_argument(
            "--gap-us belongs to closed_loop mode and --window to windowed mode");
    }
    cell.mode    = windowed ? Mode::WINDOWED : Mode::CLOSED_LOOP;
    cell.route   = route == "leaf_to_leaf" ? Route::LEAF_TO_LEAF : Route::LEAF_TO_COORDINATOR;
    cell.planned = count("--calls", "10000");
    cell.warmup  = count("--warmup", "1000");
    cell.gap_us  = windowed ? 0 : count("--gap-us", "0");
    cell.window  = windowed ? count("--window", "32") : 0;
    if (cell.planned == 0 || (windowed && cell.window == 0)) {
        throw std::invalid_argument("--calls and --window must be at least 1");
    }
    cell.handler_delay_us = count("--handler-delay-us", "0");
    cell.wrong_echo_every = count("--wrong-echo-every", "0");
    cell.throw_every      = count("--throw-every", "0");
    // The delay runs on the reader thread that dispatches the call. In the
    // leaf-to-coordinator route that is a reader of the coordinator, which a
    // validation run does not slow down.
    if (cell.handler_delay_us != 0 && cell.route == Route::LEAF_TO_COORDINATOR) {
        throw std::invalid_argument(
            "--handler-delay-us needs --route leaf_to_leaf: the coordinator is not delayed");
    }

    run_id.copy(report.header.run_id, run_id.size());
}

int run(int argc, char* argv[])
{
    parse_cell(argc, argv);
    const auto& cell = report.workload.cell;
    run_directory    = bench::run_directory();
    bench::record_pid(run_directory);
    sintra::set_log_callback(&bench::log_callback, nullptr);

    // The last branch is the caller. In the leaf-to-leaf route, branch 1 hosts
    // the callee; otherwise the coordinator does. Each branch parses the cell
    // from the coordinator's own options.
    std::vector<sintra::Process_descriptor> processes;
    if (cell.route == Route::LEAF_TO_LEAF) {
        processes.emplace_back(callee_leaf);
    }
    processes.emplace_back(caller_leaf);
    const std::vector<std::string> options(argv + 1, argv + argc);
    for (auto& process : processes) {
        process.user_options = options;
    }

    // In a branch, init runs the branch's entry function before it returns.
    // Every process writes its report at the end of its measurement protocol.
    sintra::init(argc, argv, processes);
    if (sintra::process_index() == 0) {
        run_protocol([&] { follow_caller(cell.route == Route::LEAF_TO_COORDINATOR); });
    }
    bench::shut_down_and_rethrow();
    return 0;
}


// ---------------------------------------------------------------------------
// Aggregate mode
// ---------------------------------------------------------------------------

void append_cell(bench::Json_writer& json, const rpc_cell_t& cell)
{
    bench::record_cell_t record;
    record.workload         = "rpc";
    record.mode             = mode_name(cell.mode);
    record.route            = route_name(cell.route);
    record.n_leaves         = leaf_count(cell);
    record.planned          = cell.planned;
    record.warmup           = cell.warmup;
    record.handler_delay_us = cell.handler_delay_us;
    record.validation_only  = is_validation_only(cell);
    if (cell.mode == Mode::CLOSED_LOOP) {
        record.gap_us = cell.gap_us;
    }
    else {
        record.window = cell.window;
    }
    bench::append_cell(json, record);
}

// A call is delivered when it returned a value; a call error is an RPC
// exception or a wrong value; a lost call timed out.
void append_counts(bench::Json_writer& json, const Report& caller, uint64_t eviction_log_matches)
{
    const auto& counts = caller.workload;
    json.begin_object("counts");
    json.count("published", counts.issued);
    json.count("deliveries_expected", counts.issued);
    json.count("deliveries", caller.samples.size());
    json.count("lost", counts.timed_out);
    json.null("unresolved_tail");
    json.null("gap_intervals");
    json.null("order_errors");
    json.null("size_errors");
    json.count("call_errors", counts.exceptions + counts.wrong_echoes);
    json.count("eviction_log_matches", eviction_log_matches);
    json.null("sentinel_missed");
    json.end_object();
}

void append_throughput(bench::Json_writer& json, uint64_t calls, double interval_s)
{
    json.begin_object("throughput");
    json.number("interval_s", interval_s);
    for (const char* name :
         {"delivery_interval_s", "fence_s", "publication_msg_s", "publication_app_mb_s",
          "publication_framed_mb_s", "delivery_msg_s", "delivery_app_mb_s",
          "delivery_framed_mb_s"})
    {
        json.null(name);
    }
    json.number("calls_s", (double)calls / interval_s);
    json.end_object();
}

void append_latencies(bench::Json_writer& json, const Report& caller)
{
    const auto& counts      = caller.workload;
    const bool  closed_loop = counts.cell.mode == Mode::CLOSED_LOOP;

    // Closed-loop: the idle time before a call runs from the return of the
    // call before it, or from the begin rendezvous for the first call. It is
    // unknown after a call that left no sample.
    std::vector<uint64_t> call_times, gaps;
    uint64_t              idle_begin_ns = counts.calls_begin_ns;
    uint64_t              expected      = 0;
    for (const auto& sample : caller.samples) {
        call_times.push_back(sample.end_ns - sample.begin_ns);
        if (closed_loop && sample.key == expected) {
            gaps.push_back(sample.begin_ns - idle_begin_ns);
        }
        idle_begin_ns = sample.end_ns;
        expected      = sample.key + 1;
    }

    json.begin_object("latency");
    for (const char* name :
         {"delivery_from_send", "slowest_from_send", "delivery_from_intended",
          "slowest_from_intended", "send_call", "lateness", "fence"})
    {
        json.null(name);
    }
    if (closed_loop) {
        bench::append_latency(json, "rtt", call_times, call_times.size());
        json.null("issue_to_observation");
        bench::append_latency(json, "actual_gap", gaps, gaps.size());
    }
    else {
        json.null("rtt");
        bench::append_latency(json, "issue_to_observation", call_times, call_times.size());
        json.null("actual_gap");
    }
    json.end_object();
}

int aggregate(const std::filesystem::path& directory)
{
    const auto set = bench::read_report_set<rpc_block_t>(
        directory,
        [](const rpc_cell_t& cell) { return 1 + (int)leaf_count(cell); },
        [](const rpc_cell_t& cell, int branch_index) {
            return role_name(cell.route, branch_index);
        });
    const auto& processes = set.processes;

    auto                     reasons = bench::record_reasons(set);
    std::vector<std::string> warnings;

    bench::Json_writer json;
    bench::begin_record(json, set.run_id);
    append_cell(json, set.cell);
    if (set.complete) {
        const auto& caller = *std::find_if(processes.begin(), processes.end(),
            [](const Report& process) { return has_role(process, "caller"); });
        const auto& counts = caller.workload;
        uint64_t    eviction_log_matches = 0;
        for (const auto& process : processes) {
            eviction_log_matches += process.header.eviction_log_matches;
        }
        // Every call must return its argument, and none may be lost.
        invalid_if(reasons, counts.exceptions + counts.wrong_echoes != 0, "call_errors");
        invalid_if(reasons, counts.timed_out != 0, "loss");
        invalid_if(reasons, eviction_log_matches != 0, "eviction_log_matches");

        // From the return of the caller's begin rendezvous to its end snapshot.
        const double interval_s =
            (double)(caller.header.measure_end_ns - counts.calls_begin_ns) / 1.0e9;
        bench::add_cpu_warnings(warnings, processes, interval_s);

        append_counts(json, caller, eviction_log_matches);
        append_throughput(json, counts.issued, interval_s);
        append_latencies(json, caller);
        bench::append_cpu(
            json, processes, {"coordinator", "coordinator_callee", "callee", "caller"},
            counts.issued);
    }
    else {
        json.null("counts");
        json.null("throughput");
        json.null("latency");
        json.null("cpu");
    }
    json.null("lifecycle");
    bench::end_record(json, set.missing, reasons, warnings);
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    return bench::benchmark_main(argc, argv, "rpc_benchmark", aggregate, run);
}
