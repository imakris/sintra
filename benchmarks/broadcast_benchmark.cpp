// W1, broadcast: the coordinator or a leaf publishes through `world() << message`
// and R receiver leaves handle each message in an activated slot.
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace bench = sintra::bench;

constexpr char     k_all_processes[]   = "_sintra_all_processes";
constexpr uint64_t k_warmup_sequence   = UINT64_MAX;
constexpr uint64_t k_sentinel_sequence = UINT64_MAX - 1;

// Open-loop schedule: t0 follows the begin rendezvous by 10 ms; the generator
// sleeps until 100 us before an intended arrival and then polls the clock.
constexpr uint64_t k_open_loop_lead_ns = 10'000'000;
constexpr uint64_t k_generator_poll_ns = 100'000;

// Open-loop drain: the sender stays quiet before it writes the sentinel, and a
// receiver then has this long to reach it.
constexpr auto     k_sentinel_quiet  = std::chrono::milliseconds(100);
constexpr uint64_t k_drain_timeout_s = 10;

enum class Mode : uint32_t
{
    WINDOWED,
    OPEN_LOOP,
};

enum class Origin : uint32_t
{
    COORDINATOR,
    LEAF,
};

// The cell of a sample. Every process parses it from the same options.
struct broadcast_cell_t
{
    Mode     mode;
    Origin   origin;
    uint32_t receivers;
    uint32_t reserved;
    uint64_t payload_bytes;
    uint64_t planned;
    uint64_t warmup;
    uint64_t burst_messages;
    uint64_t handler_delay_us;
    double   offered_rate;

    bool operator==(const broadcast_cell_t&) const = default;
};

// The workload block of a process report: the cell, then the sender's and the
// receiver's counters. A process fills only the counters of its role.
struct broadcast_block_t
{
    broadcast_cell_t cell;

    uint64_t published;
    uint64_t publish_begin_ns;
    uint64_t t0_ns;
    uint64_t last_send_end_ns;

    uint64_t received;
    uint64_t next_expected;
    uint64_t lost;
    uint64_t gap_intervals;
    uint64_t order_errors;
    uint64_t size_errors;
    uint64_t framed_bytes;
    uint64_t last_delivery_ns;
    uint64_t sentinel_missed;
};

using Report = bench::Process_report<broadcast_block_t>;

// This process's raw data and the run directory it is written to.
Report                report;
uint64_t              sampling_divisor = 1;
std::filesystem::path run_directory;

// A failure of this process's part of the protocol. It is kept until the
// process has left the swarm through sintra::shutdown().
std::exception_ptr failure;

// A receiver's handler thread alone writes its counters and samples. It counts
// only once `measuring` is set, so the counters start from zero at
// measure-begin. The control thread reads them only after it has deactivated
// the slot: the deactivator returns when no handler is running any more.
std::atomic<bool>     measuring{false};
std::function<void()> deactivate_slot;

// The receiver's handler thread hands the sentinel over to its control thread.
std::mutex              sentinel_mutex;
std::condition_variable sentinel_arrived;
bool                    sentinel_seen = false;

const char* mode_name(Mode mode)
{
    return mode == Mode::WINDOWED ? "windowed" : "open_loop";
}

const char* origin_name(Origin origin)
{
    return origin == Origin::COORDINATOR ? "coordinator" : "leaf";
}

const char* role_name(Origin origin, int branch_index)
{
    if (branch_index == 0) {
        return origin == Origin::COORDINATOR ? "coordinator_sender" : "coordinator";
    }
    return origin == Origin::LEAF && branch_index == 1 ? "sender" : "receivers";
}

unsigned leaf_count(const broadcast_cell_t& cell)
{
    return cell.receivers + (cell.origin == Origin::LEAF ? 1 : 0);
}

// Reserves framing headroom, so that a burst is drained before it can wrap the
// ring. Both arms of a pair include the same processing-fence cost.
uint64_t default_burst(uint64_t payload_bytes)
{
    return std::max<uint64_t>(1, (sintra::message_ring_size / 4) / (payload_bytes + 512));
}

// A sample is validation-only when an option changed the effective workload:
// a handler delay, or, in windowed mode, a measured burst other than the
// default (0 runs unpaced). Open-loop publication does not use the burst.
bool is_validation_only(const broadcast_cell_t& cell)
{
    return cell.handler_delay_us != 0 ||
        (cell.mode == Mode::WINDOWED &&
         cell.burst_messages != default_burst(cell.payload_bytes));
}

uint64_t intended_ns(const broadcast_block_t& sender, uint64_t sequence)
{
    return sender.t0_ns +
        (uint64_t)std::llround((double)sequence * 1.0e9 / sender.cell.offered_rate);
}

size_t expected_samples()
{
    // The sampled share of a run varies around planned/divisor.
    return (size_t)(report.workload.cell.planned / sampling_divisor + 4096);
}


// ---------------------------------------------------------------------------
// Run mode
// ---------------------------------------------------------------------------

// A barrier that returns no watermark was not held: the runtime treats it as
// satisfied while a process shuts down or drains. The steps that follow would
// then not mean what the protocol says, so the process fails the sample: it
// writes no report.
template <typename Barrier_mode>
void hold(const char* name)
{
    if (sintra::barrier<Barrier_mode>(name, k_all_processes) == sintra::invalid_sequence) {
        throw std::runtime_error(std::string("the barrier ") + name + " was bypassed");
    }
}

void processing_barrier(const char* name)
{
    hold<sintra::processing_fence_t>(name);
}

void rendezvous(const char* name)
{
    hold<sintra::rendezvous_t>(name);
}

// The sender times every processing fence of a windowed measurement. Its
// intervals are the burst fences in order, followed by the final fence.
void timed_fence(const char* name)
{
    const auto begin = bench::now_ns();
    processing_barrier(name);
    report.intervals.push_back({(uint64_t)report.intervals.size(), begin, bench::now_ns()});
}

// Runs this process's part of the measurement protocol.
template <typename Protocol>
void run_protocol(Protocol protocol)
{
    try {
        protocol();
    }
    catch (...) {
        failure = std::current_exception();
    }
}

// Every process calls each burst fence once, whether or not it publishes.
void join_burst_fences(uint64_t messages, uint64_t burst, const char* name)
{
    if (burst != 0) {
        for (uint64_t completed = burst; completed <= messages; completed += burst) {
            processing_barrier(name);
        }
    }
}

// Called after the warm-up fence. Every process resets and takes its begin
// snapshot before the rendezvous, and the sender publishes only after the
// rendezvous returns: no measured message precedes a reset or a snapshot.
void begin_measurement()
{
    measuring.store(true, std::memory_order_release);
    bench::mark_measure_begin(report.header);
    rendezvous("benchmark-begin");
}

// Called after the mode's end barrier. Reception ends with the end snapshot:
// the deactivator waits for a handler that is still running, as one can be
// after a missed sentinel, and no handler runs after it. No process writes its
// report or shuts down before the last rendezvous returns. The report is
// written before shutdown(), whose opening processing fence can stall on an
// evicted receiver and would otherwise lose the counts.
void end_measurement_and_report()
{
    if (deactivate_slot) {
        deactivate_slot();
    }
    bench::mark_measure_end(report.header);
    rendezvous("benchmark-end");

    const int              branch_index = sintra::process_index();
    const std::string_view role = role_name(report.workload.cell.origin, branch_index);
    role.copy(report.header.role, role.size());
    report.header.branch_index         = branch_index;
    report.header.eviction_log_matches = bench::eviction_log_matches.load();
    bench::write_report(run_directory, report);
}

// The send-start stamp is taken before the payload is filled and serialized.
uint64_t send(std::string& message, uint64_t sequence)
{
    const auto sent = bench::now_ns();
    std::memcpy(message.data(), &sequence, sizeof(sequence));
    std::memcpy(message.data() + sizeof(sequence), &sent, sizeof(sent));
    sintra::world() << message;
    return sent;
}

// The caller decides before the send whether the send end is stamped: for a
// sampled message, and for the last one, whose publish return ends the
// publication interval. The stamp is taken as soon as the publish call returns,
// before any bookkeeping.
void send_measured(std::string& message, uint64_t sequence, bool sampled, bool last)
{
    const auto sent = send(message, sequence);
    if (sampled || last) {
        const auto returned = bench::now_ns();
        if (last) {
            report.workload.last_send_end_ns = returned;
        }
        if (sampled) {
            report.samples.push_back({sequence, sent, returned});
        }
    }
}

void publish_windowed(std::string& message)
{
    const auto& cell = report.workload.cell;
    for (uint64_t i = 0; i < cell.planned; ++i) {
        send_measured(message, i, bench::is_sampled(i, sampling_divisor), i + 1 == cell.planned);
        if (cell.burst_messages != 0 && (i + 1) % cell.burst_messages == 0) {
            timed_fence("benchmark-burst");
        }
    }
    timed_fence("benchmark-complete");
}

// A late generator returns at once: it never skips a message, so backlog shows
// up as lateness.
void wait_for_intended_arrival(uint64_t intended)
{
    const auto now = bench::now_ns();
    if (now >= intended) {
        return;
    }
    if (intended - now > k_generator_poll_ns) {
        std::this_thread::sleep_until(bench::Clock::time_point(
            std::chrono::duration_cast<bench::Clock::duration>(
                std::chrono::nanoseconds(intended - k_generator_poll_ns))));
    }
    while (bench::now_ns() < intended) {
    }
}

void publish_open_loop(std::string& message)
{
    auto&       sender = report.workload;
    const auto& cell   = sender.cell;
    sender.t0_ns = sender.publish_begin_ns + k_open_loop_lead_ns;
    for (uint64_t i = 0; i < cell.planned; ++i) {
        const bool sampled = bench::is_sampled(i, sampling_divisor);
        const bool last    = i + 1 == cell.planned;
        wait_for_intended_arrival(intended_ns(sender, i));
        send_measured(message, i, sampled, last);
    }

    // A receiver that fell behind or was evicted resynchronises during the
    // quiet period, before the sentinel is written. The other processes wait in
    // the published rendezvous: a receiver's drain allowance starts when it
    // returns, however late the publication ended.
    std::this_thread::sleep_for(k_sentinel_quiet);
    send(message, k_sentinel_sequence);
    rendezvous("benchmark-published");
    rendezvous("benchmark-drained");
}

void run_sender()
{
    auto&       sender = report.workload;
    const auto& cell   = sender.cell;
    std::string message(cell.payload_bytes, 'x');
    report.samples.reserve(expected_samples());
    if (cell.mode == Mode::WINDOWED) {
        const auto bursts = cell.burst_messages != 0 ? cell.planned / cell.burst_messages : 0;
        report.intervals.reserve((size_t)bursts + 1);
    }

    // Warm-up is always paced with the default burst, also in an unpaced run.
    const auto warmup_burst = default_burst(cell.payload_bytes);
    processing_barrier("benchmark-ready");
    for (uint64_t i = 0; i < cell.warmup; ++i) {
        send(message, k_warmup_sequence);
        if ((i + 1) % warmup_burst == 0) {
            processing_barrier("benchmark-warm-burst");
        }
    }
    processing_barrier("benchmark-warm");
    begin_measurement();
    sender.publish_begin_ns = bench::now_ns();
    if (cell.mode == Mode::WINDOWED) {
        publish_windowed(message);
    }
    else {
        publish_open_loop(message);
    }
    sender.published = cell.planned;
    end_measurement_and_report();
}

// Called once the sender has written the sentinel.
void wait_for_sentinel()
{
    std::unique_lock<std::mutex> lock(sentinel_mutex);
    report.workload.sentinel_missed = !sentinel_arrived.wait_for(
        lock, std::chrono::seconds(k_drain_timeout_s), [] { return sentinel_seen; });
}

// The control-thread protocol of a process that does not publish: the
// coordinator in leaf origin, and every receiver.
void follow_sender(bool is_receiver)
{
    const auto& cell = report.workload.cell;
    processing_barrier("benchmark-ready");
    join_burst_fences(cell.warmup, default_burst(cell.payload_bytes), "benchmark-warm-burst");
    processing_barrier("benchmark-warm");
    begin_measurement();
    if (cell.mode == Mode::WINDOWED) {
        join_burst_fences(cell.planned, cell.burst_messages, "benchmark-burst");
        processing_barrier("benchmark-complete");
    }
    else {
        // The sender joins once it has published, stayed quiet and written the
        // sentinel. Only then does a receiver's drain allowance start, so a
        // late sender cannot cause a missed sentinel.
        rendezvous("benchmark-published");
        if (is_receiver) {
            wait_for_sentinel();
        }
        rendezvous("benchmark-drained");
    }
    end_measurement_and_report();
}

void on_message(const std::string& message)
{
    // The receive stamp precedes every piece of bookkeeping.
    const auto received = bench::now_ns();
    if (!measuring.load(std::memory_order_acquire)) {
        return;
    }
    const auto& cell   = report.workload.cell;
    auto&       counts = report.workload;
    if (message.size() != cell.payload_bytes || message.back() != 'x') {
        ++counts.size_errors;
        return;
    }
    uint64_t sequence = 0;
    uint64_t sent     = 0;
    std::memcpy(&sequence, message.data(), sizeof(sequence));
    std::memcpy(&sent, message.data() + sizeof(sequence), sizeof(sent));
    if (sequence == k_sentinel_sequence) {
        {
            std::lock_guard<std::mutex> lock(sentinel_mutex);
            sentinel_seen = true;
        }
        sentinel_arrived.notify_one();
        return;
    }

    if (counts.received == 0) {
        counts.framed_bytes = bench::current_frame_bytes();
    }
    if (sequence > counts.next_expected) {
        counts.lost += sequence - counts.next_expected;
        ++counts.gap_intervals;
    }
    else
    if (sequence < counts.next_expected) {
        ++counts.order_errors;
    }
    counts.next_expected = sequence + 1;
    ++counts.received;
    counts.last_delivery_ns = received;
    if (bench::is_sampled(sequence, sampling_divisor)) {
        report.samples.push_back({sequence, sent, received});
    }

    // Validation only: a slow handler. A busy wait takes the same time on every
    // platform; a sleep would follow the platform's timer granularity.
    if (cell.handler_delay_us != 0) {
        const auto resume = received + cell.handler_delay_us * 1000;
        while (bench::now_ns() < resume) {
        }
    }
}

int receiver()
{
    report.samples.reserve(expected_samples());
    deactivate_slot =
        sintra::activate_slot([](const std::string& message) { on_message(message); });
    run_protocol([] { follow_sender(true); });
    return 0;
}

int sender_leaf()
{
    run_protocol(run_sender);
    return 0;
}

void parse_cell(int argc, char* argv[])
{
    const auto text = [&](const char* flag, const char* fallback) {
        return bench::option_value(argc, argv, flag, fallback);
    };
    const auto count = [&](const char* flag, const std::string& fallback) {
        return bench::parse_count(bench::option_value(argc, argv, flag, fallback), flag);
    };
    const auto mode   = text("--mode", "windowed");
    const auto origin = text("--origin", "coordinator");
    const auto run_id = text("--run-id", "");
    if (mode != "windowed" && mode != "open_loop") {
        throw std::invalid_argument("--mode must be windowed or open_loop");
    }
    if (origin != "coordinator" && origin != "leaf") {
        throw std::invalid_argument("--origin must be coordinator or leaf");
    }
    if (run_id.empty() || run_id.size() >= sizeof(report.header.run_id)) {
        throw std::invalid_argument("--run-id must have 1..63 characters");
    }

    auto& cell = report.workload.cell;
    cell.mode             = mode == "windowed" ? Mode::WINDOWED : Mode::OPEN_LOOP;
    cell.origin           = origin == "coordinator" ? Origin::COORDINATOR : Origin::LEAF;
    cell.payload_bytes    = count("--bytes", "64");
    cell.planned          = count("--messages", "20000");
    cell.warmup           = count("--warmup", "1000");
    cell.handler_delay_us = count("--handler-delay-us", "0");
    const auto receivers  = count("--receivers", "1");
    if (receivers == 0 || receivers > 64 || cell.payload_bytes < 17 ||
        cell.payload_bytes > sintra::detail::message_frame_size_limit - 512)
    {
        throw std::invalid_argument(
            "receivers must be 1..64; payload must be 17..frame-limit-512");
    }
    if (cell.planned == 0) {
        throw std::invalid_argument("--messages must be at least 1");
    }
    cell.receivers      = (uint32_t)receivers;
    cell.burst_messages = count("--burst", std::to_string(default_burst(cell.payload_bytes)));

    const bool has_rate = bench::has_option(argc, argv, "--rate");
    if (has_rate != (cell.mode == Mode::OPEN_LOOP)) {
        throw std::invalid_argument("--rate <messages per second> belongs to open_loop mode");
    }
    if (has_rate) {
        cell.offered_rate = bench::parse_positive_number(text("--rate", ""), "--rate");
    }

    run_id.copy(report.header.run_id, run_id.size());
    sampling_divisor = bench::sampling_divisor(cell.planned);
}

int run(int argc, char* argv[])
{
    parse_cell(argc, argv);
    const auto& cell = report.workload.cell;
    run_directory    = bench::run_directory();
    bench::record_pid(run_directory);
    sintra::set_log_callback(&bench::log_callback, nullptr);

    // Branch 1 is the sender in leaf origin; every other branch is a receiver.
    // Each branch parses the cell from the coordinator's own options.
    std::vector<sintra::Process_descriptor> processes;
    if (cell.origin == Origin::LEAF) {
        processes.emplace_back(sender_leaf);
    }
    processes.insert(processes.end(), cell.receivers, sintra::Process_descriptor(receiver));
    const std::vector<std::string> options(argv + 1, argv + argc);
    for (auto& process : processes) {
        process.user_options = options;
    }

    // In a branch, init runs the branch's entry function before it returns.
    // Every process writes its report at the end of its measurement protocol.
    sintra::init(argc, argv, processes);
    if (sintra::process_index() == 0) {
        if (cell.origin == Origin::COORDINATOR) {
            run_protocol(run_sender);
        }
        else {
            run_protocol([] { follow_sender(false); });
        }
    }
    sintra::shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return 0;
}


// ---------------------------------------------------------------------------
// Aggregate mode
// ---------------------------------------------------------------------------

bool has_role(const Report& process, std::string_view role)
{
    return std::string_view(process.header.role) == role;
}

struct totals_t
{
    uint64_t deliveries           = 0;
    uint64_t lost                 = 0;
    uint64_t unresolved_tail      = 0;
    uint64_t gap_intervals        = 0;
    uint64_t order_errors         = 0;
    uint64_t size_errors          = 0;
    uint64_t eviction_log_matches = 0;
    uint64_t sentinel_missed      = 0;
    uint64_t framed_bytes         = 0;
    uint64_t last_delivery_ns     = 0;
};

totals_t total_counts(const broadcast_cell_t& cell, const std::vector<Report>& processes)
{
    totals_t totals;
    for (const auto& process : processes) {
        totals.eviction_log_matches += process.header.eviction_log_matches;
        if (!has_role(process, "receivers")) {
            continue;
        }
        const auto& receiver = process.workload;
        totals.deliveries      += receiver.received;
        totals.lost            += receiver.lost;
        totals.gap_intervals   += receiver.gap_intervals;
        totals.order_errors    += receiver.order_errors;
        totals.size_errors     += receiver.size_errors;
        totals.sentinel_missed += receiver.sentinel_missed;

        // What follows the last message a receiver saw is a final gap, unless
        // the receiver stopped before the sentinel: then that tail was still on
        // its way and is unresolved, not confirmed loss.
        const uint64_t tail =
            receiver.next_expected < cell.planned ? cell.planned - receiver.next_expected : 0;
        if (receiver.sentinel_missed != 0) {
            totals.unresolved_tail += tail;
        }
        else
        if (tail != 0) {
            totals.lost += tail;
            ++totals.gap_intervals;
        }
        totals.framed_bytes     = std::max(totals.framed_bytes, receiver.framed_bytes);
        totals.last_delivery_ns = std::max(totals.last_delivery_ns, receiver.last_delivery_ns);
    }
    return totals;
}

void append_cell(
    bench::Json_writer& json, const broadcast_cell_t& cell, std::optional<uint64_t> framed_bytes)
{
    const bool open_loop = cell.mode == Mode::OPEN_LOOP;
    json.begin_object("cell");
    json.text("workload", "broadcast");
    json.text("mode", mode_name(cell.mode));
    json.text("origin", origin_name(cell.origin));
    json.count("receivers", cell.receivers);
    json.count("payload_bytes", cell.payload_bytes);
    json.count("framed_bytes", framed_bytes);
    json.null("gap_us");
    json.null("window");
    json.count("n_leaves", leaf_count(cell));
    json.number(
        "offered_rate", open_loop ? std::optional<double>(cell.offered_rate) : std::nullopt);
    json.count("planned", cell.planned);
    json.count("warmup", cell.warmup);
    json.count("burst", cell.burst_messages);
    json.count("handler_delay_us", cell.handler_delay_us);
    json.count(
        "drain_timeout_s", open_loop ? std::optional<uint64_t>(k_drain_timeout_s) : std::nullopt);
    json.flag("validation_only", is_validation_only(cell));
    json.end_object();
}

void append_counts(bench::Json_writer& json, const Report& sender, const totals_t& totals)
{
    const auto& cell      = sender.workload.cell;
    const bool  open_loop = cell.mode == Mode::OPEN_LOOP;
    json.begin_object("counts");
    json.count("published", sender.workload.published);
    json.count("deliveries_expected", sender.workload.published * cell.receivers);
    json.count("deliveries", totals.deliveries);
    json.count("lost", totals.lost);
    json.count("unresolved_tail", totals.unresolved_tail);
    json.count("gap_intervals", totals.gap_intervals);
    json.count("order_errors", totals.order_errors);
    json.count("size_errors", totals.size_errors);
    json.null("call_errors");
    json.count("eviction_log_matches", totals.eviction_log_matches);
    json.count(
        "sentinel_missed",
        open_loop ? std::optional<uint64_t>(totals.sentinel_missed) : std::nullopt);
    json.end_object();
}

// Windowed: one interval, from the return of the sender's begin rendezvous to
// its end snapshot. Open-loop: publication runs from t0 until the last measured
// publish call returns, delivery from t0 to the latest handler stamp of a
// measured message.
struct measured_seconds_t
{
    double publication;
    double delivery;
};

measured_seconds_t measured_seconds(const Report& sender, const totals_t& totals)
{
    const auto& sent    = sender.workload;
    const auto  seconds = [](uint64_t begin_ns, uint64_t end_ns) {
        return (double)(end_ns - begin_ns) / 1.0e9;
    };
    if (sent.cell.mode == Mode::WINDOWED) {
        const auto interval = seconds(sent.publish_begin_ns, sender.header.measure_end_ns);
        return {interval, interval};
    }
    return {
        seconds(sent.t0_ns, sent.last_send_end_ns),
        totals.deliveries != 0 ? seconds(sent.t0_ns, totals.last_delivery_ns) : std::nan(""),
    };
}

void append_throughput(
    bench::Json_writer&        json,
    const Report&              sender,
    const totals_t&            totals,
    const measured_seconds_t&  measured)
{
    const auto payload_bytes = sender.workload.cell.payload_bytes;

    // Windowed: the interval, and so the rates and the CPU per message, include
    // the sender's time in its burst fences and in the final fence.
    std::optional<double> fence_seconds;
    if (sender.workload.cell.mode == Mode::WINDOWED) {
        uint64_t fence_ns = 0;
        for (const auto& fence : sender.intervals) {
            fence_ns += fence.end_ns - fence.begin_ns;
        }
        fence_seconds = (double)fence_ns / 1.0e9;
    }

    const auto rates = [&](const std::string& name, uint64_t messages, double seconds) {
        const double per_second = (double)messages / seconds;
        json.number(name + "_msg_s", per_second);
        json.number(name + "_app_mb_s", per_second * (double)payload_bytes / 1.0e6);
        json.number(
            name + "_framed_mb_s",
            totals.framed_bytes != 0
                ? std::optional<double>(per_second * (double)totals.framed_bytes / 1.0e6)
                : std::nullopt);
    };
    json.begin_object("throughput");
    json.number("interval_s", measured.publication);
    json.number("delivery_interval_s", measured.delivery);
    json.number("fence_s", fence_seconds);
    rates("publication", sender.workload.published, measured.publication);
    rates("delivery", totals.deliveries, measured.delivery);
    json.null("calls_s");
    json.end_object();
}

void append_latencies(
    bench::Json_writer& json, const Report& sender, const std::vector<Report>& processes)
{
    const auto& cell      = sender.workload.cell;
    const bool  open_loop = cell.mode == Mode::OPEN_LOOP;

    // Join the receivers' samples on the sequence.
    std::vector<bench::sample_t> deliveries;
    for (const auto& process : processes) {
        if (has_role(process, "receivers")) {
            deliveries.insert(deliveries.end(), process.samples.begin(), process.samples.end());
        }
    }
    std::sort(
        deliveries.begin(), deliveries.end(),
        [](const bench::sample_t& a, const bench::sample_t& b) { return a.key < b.key; });

    std::vector<uint64_t> from_send, from_intended, slowest_from_send, slowest_from_intended;
    uint64_t              messages = 0;
    for (size_t first = 0; first < deliveries.size();) {
        const auto sequence = deliveries[first].key;
        const auto sent     = deliveries[first].begin_ns;
        const auto intended = open_loop ? intended_ns(sender.workload, sequence) : 0;
        uint64_t   slowest  = 0;
        size_t     last     = first;
        for (; last < deliveries.size() && deliveries[last].key == sequence; ++last) {
            const auto received = deliveries[last].end_ns;
            from_send.push_back(received - sent);
            if (open_loop) {
                from_intended.push_back(received - intended);
            }
            slowest = std::max(slowest, received);
        }
        ++messages;
        // The slowest receiver is defined only for a message that reached all.
        if (last - first == cell.receivers) {
            slowest_from_send.push_back(slowest - sent);
            if (open_loop) {
                slowest_from_intended.push_back(slowest - intended);
            }
        }
        first = last;
    }

    std::vector<uint64_t> send_call, lateness, fence;
    for (const auto& sample : sender.samples) {
        send_call.push_back(sample.end_ns - sample.begin_ns);
        if (open_loop) {
            lateness.push_back(sample.begin_ns - intended_ns(sender.workload, sample.key));
        }
    }
    // The sender's last interval is the final fence, which is not a burst fence.
    for (size_t i = 0; i + 1 < sender.intervals.size(); ++i) {
        fence.push_back(sender.intervals[i].end_ns - sender.intervals[i].begin_ns);
    }

    // Pooled deliveries have more observations than distinct messages; every
    // other object has one observation per message, send or burst.
    json.begin_object("latency");
    bench::append_latency(json, "delivery_from_send", from_send, messages);
    bench::append_latency(json, "slowest_from_send", slowest_from_send, slowest_from_send.size());
    if (open_loop) {
        bench::append_latency(json, "delivery_from_intended", from_intended, messages);
        bench::append_latency(
            json, "slowest_from_intended", slowest_from_intended, slowest_from_intended.size());
    }
    else {
        json.null("delivery_from_intended");
        json.null("slowest_from_intended");
    }
    bench::append_latency(json, "send_call", send_call, send_call.size());
    if (open_loop) {
        bench::append_latency(json, "lateness", lateness, lateness.size());
        json.null("fence");
    }
    else {
        json.null("lateness");
        bench::append_latency(json, "fence", fence, fence.size());
    }
    json.null("rtt");
    json.null("issue_to_observation");
    json.null("actual_gap");
    json.end_object();
}

double process_cpu_s(const Report& process)
{
    return process.header.cpu_end_s - process.header.cpu_begin_s;
}

void append_cpu(
    bench::Json_writer&         json,
    const std::vector<Report>&  processes,
    uint64_t                    published,
    uint64_t                    deliveries)
{
    json.begin_object("cpu");
    bench::Role_cpu total{"total"};
    for (const std::string_view name :
         {"coordinator", "coordinator_sender", "sender", "receivers"})
    {
        bench::Role_cpu role{std::string(name)};
        for (const auto& process : processes) {
            if (has_role(process, name)) {
                bench::add_process_cpu(role, process_cpu_s(process));
                bench::add_process_cpu(total, process_cpu_s(process));
            }
        }
        if (role.processes != 0) {
            bench::append_role_cpu(
                json, role, published,
                name == "receivers" ? std::optional<uint64_t>(deliveries) : std::nullopt);
        }
    }
    bench::append_role_cpu(json, total, published);
    json.end_object();
}

void append_names(
    bench::Json_writer& json, std::string_view key, const std::vector<std::string>& names)
{
    json.begin_array(key);
    for (const auto& name : names) {
        json.text({}, name);
    }
    json.end_array();
}

int aggregate(const std::filesystem::path& directory)
{
    auto processes = bench::read_reports<broadcast_block_t>(directory);
    if (processes.empty()) {
        throw std::runtime_error("no process reports in " + directory.string());
    }

    // The coordinator's report names the run and the cell. A report of another
    // run or of another cell is dropped, so the process it stands for counts
    // as missing.
    const auto coordinator = std::find_if(processes.begin(), processes.end(),
        [](const Report& process) { return process.header.branch_index == 0; });
    const auto& reference = coordinator != processes.end() ? *coordinator : processes.front();
    const auto  run_id    = std::string(reference.header.run_id);
    const auto  cell      = reference.workload.cell;
    const auto  foreign   = std::erase_if(processes,
        [&](const Report& process) { return process.header.run_id != run_id; });
    const auto  inconsistent = std::erase_if(processes,
        [&](const Report& process) { return !(process.workload.cell == cell); });

    // The cell expects exactly one report per process. Whatever remains beyond
    // the expected ones is a duplicate or belongs to a branch the cell lacks.
    const int                expected = 1 + (int)leaf_count(cell);
    std::vector<std::string> missing;
    for (int branch_index = 0; branch_index < expected; ++branch_index) {
        const std::string_view role  = role_name(cell.origin, branch_index);
        const bool             found = std::any_of(processes.begin(), processes.end(),
            [&](const Report& process) {
                return process.header.branch_index == branch_index && has_role(process, role);
            });
        if (!found) {
            missing.push_back(bench::report_file_name(role, branch_index));
        }
    }
    const bool unexpected = processes.size() != (size_t)expected - missing.size();

    auto                     reasons = bench::build_invalid_reasons();
    std::vector<std::string> warnings;
    const auto invalid_if = [&](bool condition, const char* reason) {
        if (condition) {
            reasons.push_back(reason);
        }
    };
    invalid_if(!missing.empty(), "missing_files");
    invalid_if(foreign != 0, "foreign_run_files");
    invalid_if(inconsistent != 0, "inconsistent_reports");
    invalid_if(unexpected, "unexpected_reports");

    bench::Json_writer json;
    json.begin_object();
    json.text("schema", "sintra-bench/1");
    json.text("run_id", run_id);
    bench::append_build_facts(json, "build");
    if (missing.empty() && !unexpected) {
        const auto& sender = *std::find_if(processes.begin(), processes.end(),
            [](const Report& process) {
                return has_role(process, "coordinator_sender") || has_role(process, "sender");
            });
        const auto totals   = total_counts(cell, processes);
        const auto measured = measured_seconds(sender, totals);
        const bool windowed = cell.mode == Mode::WINDOWED;
        invalid_if(totals.order_errors != 0, "order_errors");
        invalid_if(totals.size_errors != 0, "size_errors");
        // Loss invalidates a windowed sample. In open-loop mode it is a result.
        invalid_if(windowed && totals.lost != 0, "loss");
        invalid_if(windowed && totals.eviction_log_matches != 0, "eviction_log_matches");
        invalid_if(!windowed && totals.sentinel_missed != 0, "sentinel_missed");

        const double achieved_rate = (double)sender.workload.published / measured.publication;
        if (measured.publication < 2.0) {
            warnings.push_back("cpu_low_confidence");
        }
        // A process CPU clock that did not move over the interval says nothing
        // about the role's cost.
        if (std::any_of(processes.begin(), processes.end(),
                [](const Report& process) { return process_cpu_s(process) == 0; }))
        {
            warnings.push_back("cpu_not_advancing");
        }
        if (!windowed && achieved_rate < 0.95 * cell.offered_rate) {
            warnings.push_back("rate_shortfall");
        }

        append_cell(
            json, cell,
            totals.framed_bytes != 0 ? std::optional<uint64_t>(totals.framed_bytes) : std::nullopt);
        append_counts(json, sender, totals);
        append_throughput(json, sender, totals, measured);
        append_latencies(json, sender, processes);
        append_cpu(json, processes, sender.workload.published, totals.deliveries);
    }
    else {
        append_cell(json, cell, std::nullopt);
        json.null("counts");
        json.null("throughput");
        json.null("latency");
        json.null("cpu");
    }
    json.null("lifecycle");
    json.begin_object("outcome");
    append_names(json, "missing_files", missing);
    json.flag("valid", reasons.empty());
    append_names(json, "reasons", reasons);
    append_names(json, "warnings", warnings);
    json.end_object();
    json.end_object();
    std::printf("%s\n", json.str().c_str());
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
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
        return run(argc, argv);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "broadcast_benchmark: %s\n", error.what());
        return 1;
    }
}
