#include <sintra/sintra.h>

#include "exact_child_test_support.h"
#include "test_ring_utils.h"
#include "test_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace detail = sintra::detail;
using sintra::test::Exact_child;
using sintra::test::Exact_child_state;
using Reader = sintra::Ring_R<uint32_t>;
using Writer = sintra::Ring_W<uint32_t>;
using History = detail::Ring_retained_history<uint32_t>;
using Notification = detail::Ring_native_notification<uint32_t>;
using Native_record = detail::ring_native_notification_record_t;

class Guard_reader : public Reader
{
public:
    using Reader::Reader;
    uint64_t guard_counts() const { return this->m_control->read_access.load(); }
};

constexpr const char* k_ring_name = "retained_history";
constexpr uint64_t k_occurrence = 1;
constexpr uint32_t k_poison = 0xbad0c0deu;
constexpr auto k_deadline = std::chrono::seconds(12);

struct history_case_t
{
    const char* name;
    bool publish_small;
    bool kill_at_head;
    bool kill_recoverer;
    bool framed = false;
    bool replay_at_release = false;
};

constexpr history_case_t k_cases[] = {
    {"before_head", false, false, false},
    {"after_head", true, true, false},
    {"smaller_commit", true, false, false},
    {"recovery_copy_death", true, false, true},
    {"framed_prefix", true, false, false, true},
    {"framed_release_race", true, false, false, true, true},
    {"framed_before_saved_marker", false, false, false, true},
    {"framed_after_saved_marker", false, false, false, true},
    {"framed_before_exposure", false, false, false, true},
    {"framed_batch_wrap", true, false, false, true},
    {"framed_partial_wrap", false, false, false, true},
    {"framed_validation", true, false, false, true},
    {"framed_versions", true, false, false, true},
    {"framed_recovery_copy_death", true, false, true, true},
    {"protected_guard", false, false, false},
};

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::fprintf(stderr, "retained history check failed: %s\n", message.c_str());
        throw std::runtime_error(message);
    }
}

template <typename Predicate>
bool await(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + k_deadline;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void signal_file(const fs::path& path)
{
    std::ofstream stream(path);
    stream << "ready\n";
    stream.close();
    require(bool(stream), "cannot publish phase: " + path.string());
}

void wait_for_file(const fs::path& path)
{
    require(await([&] { return fs::exists(path); }), "phase deadline: " + path.string());
}

const history_case_t& find_case(std::string_view name)
{
    for (const auto& test : k_cases) {
        if (name == test.name) {
            return test;
        }
    }
    throw std::runtime_error("unknown retained history case: " + std::string(name));
}

uint32_t payload(size_t sequence)
{
    return uint32_t(0x13570000u + sequence);
}

void publish_instance(const fs::path& directory, const char* role)
{
    std::ofstream stream(directory / (std::string(role) + "_instance"));
    stream << detail::current_process_instance();
    stream.close();
    require(bool(stream), "cannot publish native child instance");
    signal_file(directory / (std::string(role) + "_armed"));
}

uint64_t captured_instance(const fs::path& directory, const char* role, const Exact_child& child)
{
    uint64_t instance = 0;
    std::ifstream stream(directory / (std::string(role) + "_instance"));
    stream >> instance;
    require(bool(stream) && detail::process_instance_pid(instance) == uint32_t(child.pid()),
        "captured instance does not identify the retained native child");
    return instance;
}

std::shared_ptr<sintra::ipc::mapped_region> map_storage(const fs::path& path)
{
    sintra::ipc::file_mapping file(path, sintra::ipc::read_write);
    return std::make_shared<sintra::ipc::mapped_region>(file, sintra::ipc::read_write, 0, 0);
}

std::shared_ptr<sintra::ipc::mapped_region> create_storage(const fs::path& path, size_t bytes)
{
    std::ofstream stream(path, std::ios::binary);
    const std::vector<char> zeroes(bytes, 0);
    stream.write(zeroes.data(), std::streamsize(zeroes.size()));
    stream.close();
    require(bool(stream), "cannot allocate capability storage");
    return map_storage(path);
}

std::shared_ptr<Native_record> map_record(const fs::path& directory, const char* name)
{
    auto storage = map_storage(directory / name);
    return std::shared_ptr<Native_record>(storage, static_cast<Native_record*>(storage->data()));
}

class Armed_child_witness : public detail::Native_exit_witness
{
public:
    Armed_child_witness(uint64_t instance, std::shared_ptr<Exact_child> child)

    :
        Native_exit_witness(instance),
        m_child(std::move(child))
    {}

    bool has_exited() const noexcept override
    {
        return m_child->observe_exit_retained() == Exact_child_state::exited;
    }

private:
    const std::shared_ptr<Exact_child> m_child;
};

class Published_child_witness : public detail::Native_exit_witness
{
public:
    explicit Published_child_witness(std::shared_ptr<Native_record> record)

    :
        Native_exit_witness(record->writer_instance),
        m_record(std::move(record))
    {}

    bool has_exited() const noexcept override { return m_record->native_exit.load(); }

private:
    // Only the parent with the original native child authority publishes this.
    const std::shared_ptr<Native_record> m_record;
};

std::shared_ptr<const detail::Native_exit_authority> child_authority(
    const fs::path& directory, bool has_recoverer)
{
    std::vector<std::shared_ptr<const detail::Native_exit_witness>> witnesses{
        std::make_shared<Published_child_witness>(map_record(directory, "writer_record"))};
    if (has_recoverer) {
        witnesses.push_back(std::make_shared<Published_child_witness>(
            map_record(directory, "recoverer_record")));
    }
    return std::make_shared<detail::Native_exit_authority>(std::move(witnesses));
}

void kill_retaining_authority(const std::shared_ptr<Exact_child>& child)
{
    require(child->terminate_retaining_authority() &&
        await([&] { return child->observe_exit_retained() == Exact_child_state::exited; }),
        "cannot retain exact native death authority");
}

fs::path s_directory;
std::string s_death_stage;
bool s_poison_unpublished_save = false;

void death_hook(const char* stage, const void* object)
{
    if (s_death_stage != stage) {
        return;
    }
    if (s_poison_unpublished_save && std::string_view(stage) == "history_before_saved_marker") {
        // This marker is still unset. Recovery must ignore these saved bytes.
        std::memset(const_cast<void*>(object), 0xa5, 16);
    }
    signal_file(s_directory / "death_seam");
    wait_for_file(s_directory / "unexpected_release");
}

struct Placement_failure {};

class Throwing_placement
{
public:
    explicit Throwing_placement(size_t count)
    {
        std::fill_n(reinterpret_cast<uint32_t*>(this), count, k_poison);
        throw Placement_failure{};
    }

private:
    uint32_t m_payload;
};

struct frame_body_t
{
    uint64_t sequence;
    sintra::message_string text;
};

using Frame = sintra::Message<frame_body_t, void, 0x71a4e21u>;

unsigned s_probe_constructions = 0;

struct alignas(16) Construction_probe
{
    Construction_probe() { ++s_probe_constructions; }
    uint32_t bytes_to_next_message = sizeof(Construction_probe);
};

struct alignas(8192) Overaligned_probe
{
    Overaligned_probe() { ++s_probe_constructions; }
    uint32_t bytes_to_next_message = sizeof(Overaligned_probe);
};

class Throwing_frame_body
{
public:
    Throwing_frame_body()
    {
        m_bytes.fill(char(0xa5));
        throw Placement_failure{};
    }

private:
    std::array<char, 32768> m_bytes;
};

using Throwing_frame = sintra::Message<Throwing_frame_body, void, 0x71a4e22u>;
using Frame_history = detail::Ring_retained_history<char>;
using Frame_notification = detail::Ring_native_notification<char>;
constexpr const char* k_frame_prefix = "history_req";
constexpr uint64_t k_frame_writer_id = 11;
constexpr size_t k_frame_bytes = 4096;
constexpr size_t k_small_frame_bytes = 6144;
constexpr size_t k_initial_frames = 4;
constexpr size_t k_batch_bytes = k_frame_bytes + k_small_frame_bytes;
constexpr size_t k_batch_count = 4 * sintra::message_ring_size / k_batch_bytes + 2;

bool batch_wrap(const history_case_t& test)
{
    return std::string_view(test.name) == "framed_batch_wrap";
}

size_t frame_size(const history_case_t& test, size_t sequence)
{
    return batch_wrap(test) && sequence % 2 ? k_small_frame_bytes : k_frame_bytes;
}

size_t frame_start(const history_case_t& test, size_t sequence)
{
    return batch_wrap(test) ? sequence / 2 * k_batch_bytes + (sequence % 2) * k_frame_bytes
        : sequence * k_frame_bytes;
}

Frame_notification* s_frame_notification = nullptr;
unsigned s_frame_release_calls = 0;
bool s_frame_release_was_pending = false;
bool s_frame_release_replayed = false;

void frame_release_hook(const char* stage, const void*)
{
    if (std::string_view(stage) != "message_history_before_range_release") {
        return;
    }
    ++s_frame_release_calls;
    s_frame_release_was_pending = !s_frame_notification->retained_history()->ready();
    s_frame_release_replayed = s_frame_notification->replay(k_occurrence).state ==
        Frame_notification::Replay_state::COMPLETE;
}

std::string frame_text(size_t sequence, size_t bytes)
{
    require(bytes >= sizeof(Frame), "frame reservation cannot hold the typed message");
    return std::string(bytes - sizeof(Frame), char('a' + sequence % 26));
}

void write_frame(sintra::Message_ring_W& writer, size_t sequence, size_t bytes)
{
    const auto text = frame_text(sequence, bytes);
    const auto extra = sintra::vb_size<Frame>(uint64_t(sequence), text);
    require(sizeof(Frame) + extra == bytes, "typed frame reservation is not exactly aligned");
    auto* frame = writer.write<Frame>(extra, uint64_t(sequence), text);
    require(frame->bytes_to_next_message == bytes, "constructed frame changed its reservation size");
    writer.done_writing();
}

void check_frame(const sintra::Message_prefix* prefix, size_t sequence, size_t bytes)
{
    require(prefix && prefix->magic == sintra::message_magic &&
        prefix->message_type_id == Frame::id() && prefix->bytes_to_next_message == bytes,
        "recovered typed frame has a damaged prefix");
    const auto* frame = static_cast<const Frame*>(prefix);
    require(frame->sequence == sequence && static_cast<std::string>(frame->text) == frame_text(sequence, bytes),
        "recovered typed frame has the wrong sequence or variable payload");
}

void write_frame_batch(sintra::Message_ring_W& writer, size_t sequence)
{
    alignas(Frame) std::array<char, k_batch_bytes> bytes;
    auto* first = new (bytes.data()) Frame(uint64_t(sequence), frame_text(sequence, k_frame_bytes));
    auto* second = new (bytes.data() + k_frame_bytes) Frame(uint64_t(sequence + 1),
        frame_text(sequence + 1, k_small_frame_bytes));
    writer.write(bytes.data(), bytes.size());
    writer.done_writing();
    second->~Frame();
    first->~Frame();
}

template <typename F>
void require_rejection(F&& action)
{
    bool rejected = false;
    try { action(); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "malformed retained write was not rejected");
}

void check_frame_validation(sintra::Message_ring_W& writer)
{
    auto& raw = static_cast<sintra::Ring_W<char>&>(writer);
    const std::array<char, 16> invalid{};
    require_rejection([&] { writer.write_commit(char(0)); });
    require_rejection([&] { writer.write<Construction_probe>(1); });
    require_rejection([&] { writer.try_write_notification<Construction_probe>(1); });
    require_rejection([&] { writer.write<Construction_probe>(std::numeric_limits<size_t>::max()); });
    raw.write(invalid.data(), invalid.size());
    require_rejection([&] { writer.write<Overaligned_probe>(0); });
    require(s_probe_constructions == 0, "rejected alignment reached placement construction");
    require_rejection([&] { writer.done_writing(); });

    // A failed batch leaves a previous frame receipt at the same absolute
    // sequence. Raw bytes from the next attempt must not reuse that receipt.
    const auto text = frame_text(0, k_frame_bytes);
    writer.write<Frame>(sintra::vb_size<Frame>(uint64_t(0), text), uint64_t(0), text);
    require_rejection([&] { raw.write(invalid.data(), 1); });
    raw.write(invalid.data(), invalid.size());
    require_rejection([&] { writer.done_writing(); });
    const std::vector<char> stale_frame(k_frame_bytes, 0);
    raw.write(stale_frame.data(), stale_frame.size());
    require_rejection([&] { writer.done_writing(); });
    require(writer.get_leading_sequence() == 0, "rejected batch published a malformed head");
}

void check_frame_versions(const fs::path& directory,
    const std::shared_ptr<const detail::Native_exit_authority>& authority)
{
    auto storage = map_storage(directory / "history");
    auto* version = static_cast<uint64_t*>(storage->data());
    const auto current = *version;
    require(current == 2 && storage->size() == Frame_history::storage_size(sintra::message_ring_size, true),
        "framed capability does not identify the compact history format");
    const auto filename = sintra::get_base_filename(k_frame_prefix, k_frame_writer_id, k_occurrence);
    for (const uint64_t unsupported : {uint64_t(1), uint64_t(3)}) {
        *version = unsupported;
        bool rejected = false;
        unsigned writers_created = 0;
        try {
            auto opened = std::make_shared<Frame_history>(directory.string(), filename,
                sintra::message_ring_size, storage, authority);
            sintra::Message_ring_W writer(directory.string(), k_frame_prefix, k_frame_writer_id,
                k_occurrence, authority, opened);
            ++writers_created;
        }
        catch (const sintra::ring_acquisition_failure_exception&) { rejected = true; }
        *version = current;
        require(rejected && writers_created == 0,
            "unsupported history format reached an actual message writer");
    }
    auto opened = std::make_shared<Frame_history>(directory.string(), filename,
        sintra::message_ring_size, storage, authority);
    require(opened->compact(), "valid compact history no longer opens after rejected versions");
}

int run_frame_writer(const history_case_t& test, const fs::path& directory)
{
    publish_instance(directory, "writer");
    wait_for_file(directory / "capabilities_ready");
    const auto filename = sintra::get_base_filename(k_frame_prefix, k_frame_writer_id, k_occurrence);
    auto authority = child_authority(directory, test.kill_recoverer);
    auto history = std::make_shared<Frame_history>(directory.string(), filename,
        sintra::message_ring_size, map_storage(directory / "history"), authority);
    if (std::string_view(test.name) == "framed_versions") {
        check_frame_versions(directory, authority);
    }
    sintra::Message_ring_W writer(directory.string(), k_frame_prefix, k_frame_writer_id,
        k_occurrence, authority, history);
    if (std::string_view(test.name) == "framed_validation") {
        check_frame_validation(writer);
    }
    for (size_t sequence = 0; sequence < k_initial_frames;) {
        if (batch_wrap(test)) {
            write_frame_batch(writer, sequence);
            sequence += 2;
        }
        else {
            write_frame(writer, sequence++, k_frame_bytes);
        }
    }
    signal_file(directory / "first_committed");
    wait_for_file(directory / "readers_ready");
    const auto frame_count = batch_wrap(test) ? 2 * k_batch_count : sintra::message_ring_size / k_frame_bytes;
    bool crossed_frame = false;
    for (size_t sequence = k_initial_frames; sequence < frame_count;) {
        if (batch_wrap(test)) {
            const auto position = writer.get_leading_sequence() % sintra::message_ring_size;
            crossed_frame |= position + k_frame_bytes > sintra::message_ring_size ||
                (position + k_frame_bytes) % sintra::message_ring_size + k_small_frame_bytes >
                    sintra::message_ring_size;
            write_frame_batch(writer, sequence);
            sequence += 2;
        }
        else {
            write_frame(writer, sequence++, k_frame_bytes);
        }
    }
    require(writer.get_leading_sequence() == frame_start(test, frame_count),
        "frames did not reach the intended committed boundary");
    require(!batch_wrap(test) || (crossed_frame && writer.get_leading_sequence() > 4 * sintra::message_ring_size),
        "imported frames did not straddle the physical ring end across four laps");
    s_directory = directory;
    if (std::string_view(test.name) == "framed_before_saved_marker" ||
        std::string_view(test.name) == "framed_after_saved_marker" ||
        std::string_view(test.name) == "framed_before_exposure")
    {
        s_poison_unpublished_save = std::string_view(test.name) == "framed_before_saved_marker";
        s_death_stage = s_poison_unpublished_save ? "history_before_saved_marker"
            : std::string_view(test.name) == "framed_after_saved_marker" ? "history_preserved"
            : "history_before_exposure";
        detail::test_hooks::s_ipc_wakeup_operation = death_hook;
    }
    if (std::string_view(test.name) == "framed_partial_wrap") {
        // Exercise the preservation primitive with a partial failed overwrite
        // crossing the physical end, then retry within an already saved block.
        const auto capacity = sintra::message_ring_size;
        auto storage = map_storage(directory / filename);
        auto* original = static_cast<char*>(storage->data());
        history->preserve(capacity - 7, 30, capacity);
        std::fill_n(original + capacity - 7, 7, char(0xa5));
        std::fill_n(original, 23, char(0xa5));
        history->preserve(capacity + 5, 3, capacity);
        std::fill_n(original + 5, 3, char(0xb6));
        signal_file(directory / "writer_seam");
        wait_for_file(directory / "unexpected_release");
        return 1;
    }
    bool placement_failed = false;
    try {
        writer.write<Throwing_frame>(sintra::vb_size<Throwing_frame>());
    }
    catch (const Placement_failure&) {
        placement_failed = true;
    }
    require(placement_failed, "larger typed frame did not mutate and throw");
    // The smaller successful relay exercises the buffer boundary recorder as
    // well as the typed construction recorder used by the earlier frames.
    alignas(detail::message_frame_alignment) std::array<char, k_small_frame_bytes> relay_storage;
    const auto text = frame_text(frame_count, k_small_frame_bytes);
    auto* relayed = new (relay_storage.data()) Frame(uint64_t(frame_count), text);
    if (test.replay_at_release) {
        std::vector<char> unaligned(k_small_frame_bytes + 1);
        std::memcpy(unaligned.data() + 1, relay_storage.data(), k_small_frame_bytes);
        require(reinterpret_cast<uintptr_t>(unaligned.data() + 1) % alignof(sintra::Message_prefix) != 0,
            "buffer fixture did not offset the source prefix alignment");
        writer.write(unaligned.data() + 1, k_small_frame_bytes);
        writer.done_writing();
    }
    else {
        writer.relay(*relayed);
    }
    relayed->~Frame();
    signal_file(directory / "writer_seam");
    wait_for_file(directory / "unexpected_release");
    return 1;
}

int run_writer(const history_case_t& test, const fs::path& directory)
{
    if (test.framed) {
        return run_frame_writer(test, directory);
    }
    s_directory = directory;
    publish_instance(directory, "writer");
    wait_for_file(directory / "capabilities_ready");
    const auto capacity = sintra::test::pick_ring_elements<uint32_t>();
    auto authority = child_authority(directory, test.kill_recoverer);
    auto history = std::make_shared<History>(directory.string(), k_ring_name, capacity,
        map_storage(directory / "history"), authority);
    Writer writer(directory.string(), k_ring_name, capacity,
        detail::ring_directory_policy::caller_directory, false, authority, history);
    writer.write_commit(payload(0));
    signal_file(directory / "first_committed");
    wait_for_file(directory / "readers_ready");

    if (std::string_view(test.name) == "protected_guard") {
        writer.write_commit(payload(1));
        signal_file(directory / "writer_seam");
        wait_for_file(directory / "unexpected_release");
        return 1;
    }

    for (size_t sequence = 1; sequence < capacity; ++sequence) {
        writer.write_commit(payload(sequence));
    }
    bool placement_failed = false;
    try {
        writer.write<Throwing_placement>(capacity / 8 - 1, capacity / 8);
    }
    catch (const Placement_failure&) {
        placement_failed = true;
    }
    require(placement_failed, "larger in-place write did not mutate and throw");
    const auto smaller_value = test.publish_small ? payload(capacity) : k_poison;
    writer.write(&smaller_value, 1);
    if (test.publish_small) {
        if (test.kill_at_head) {
            s_death_stage = "ring_commit_published";
            detail::test_hooks::s_ipc_wakeup_operation = death_hook;
        }
        writer.done_writing();
    }
    signal_file(directory / "writer_seam");
    wait_for_file(directory / "unexpected_release");
    return 1;
}

int run_recoverer(const history_case_t& test, const fs::path& directory)
{
    s_directory = directory;
    publish_instance(directory, "recoverer");
    wait_for_file(directory / "recovery_allowed");
    auto authority = child_authority(directory, true);
    auto record = map_record(directory, "writer_record");
    s_death_stage = "history_image_copy";
    detail::test_hooks::s_ipc_wakeup_operation = death_hook;
    if (test.framed) {
        const auto filename = sintra::get_base_filename(k_frame_prefix, k_frame_writer_id, k_occurrence);
        auto history = std::make_shared<Frame_history>(directory.string(), filename,
            sintra::message_ring_size, map_storage(directory / "history"), authority);
        Frame_notification notification(directory.string(), filename, sintra::message_ring_size,
            record, authority, detail::ring_directory_policy::private_existing_directory,
            std::function<void()>{}, history);
        notification.replay(k_occurrence);
        throw std::runtime_error("framed recoverer missed its image-copy death seam");
    }
    const auto capacity = sintra::test::pick_ring_elements<uint32_t>();
    auto history = std::make_shared<History>(directory.string(), k_ring_name, capacity,
        map_storage(directory / "history"), authority);
    Notification notification(directory.string(), k_ring_name, capacity, record, authority,
        detail::ring_directory_policy::caller_directory, std::function<void()>{}, history);
    notification.replay(k_occurrence);
    throw std::runtime_error("recoverer missed its image-copy death seam");
}

void check_suffix(const sintra::Range<uint32_t>& range, size_t first, size_t head)
{
    require(range.begin && range.end - range.begin == std::ptrdiff_t(head - first),
        "recovered range has the wrong surviving interval");
    for (size_t sequence = first; sequence < head; ++sequence) {
        const auto value = range.begin[sequence - first];
        require(value == payload(sequence), "wrong recovered payload at sequence " +
            std::to_string(sequence) + ": " + std::to_string(value));
    }
}

int run_frame_scenario(const std::string& binary, const history_case_t& test, const fs::path& directory)
{
    const auto path = directory.string();
    const auto filename = sintra::get_base_filename(k_frame_prefix, k_frame_writer_id, k_occurrence);
    auto writer = std::make_shared<Exact_child>(std::chrono::seconds(3));
    const char* writer_args[] = {binary.c_str(), "--writer", test.name, path.c_str(), nullptr};
    require(writer->spawn(binary.c_str(), writer_args), "cannot spawn exact message writer");
    wait_for_file(directory / "writer_armed");
    const auto instance = captured_instance(directory, "writer", *writer);
    std::vector<std::shared_ptr<const detail::Native_exit_witness>> witnesses{
        std::make_shared<Armed_child_witness>(instance, writer)};
    std::shared_ptr<Exact_child> recoverer;
    std::shared_ptr<Native_record> recoverer_record;
    if (test.kill_recoverer) {
        recoverer = std::make_shared<Exact_child>(std::chrono::seconds(3));
        const char* args[] = {binary.c_str(), "--recoverer", test.name, path.c_str(), nullptr};
        require(recoverer->spawn(binary.c_str(), args), "cannot spawn exact framed recoverer");
        wait_for_file(directory / "recoverer_armed");
        const auto recoverer_instance = captured_instance(directory, "recoverer", *recoverer);
        auto recoverer_storage = create_storage(directory / "recoverer_record", sizeof(Native_record));
        recoverer_record = std::shared_ptr<Native_record>(recoverer_storage,
            new (recoverer_storage->data()) Native_record(2, recoverer_instance));
        witnesses.push_back(std::make_shared<Armed_child_witness>(recoverer_instance, recoverer));
    }
    auto authority = std::make_shared<detail::Native_exit_authority>(std::move(witnesses));
    auto record_storage = create_storage(directory / "writer_record", sizeof(Native_record));
    auto record = std::shared_ptr<Native_record>(record_storage,
        new (record_storage->data()) Native_record(k_occurrence, instance));
    const auto bytes = Frame_history::storage_size(sintra::message_ring_size, true);
    auto storage = create_storage(directory / "history", bytes);
    Frame_history::initialize(storage->data(), bytes, sintra::message_ring_size,
        k_occurrence, instance, true);
    auto history = std::make_shared<Frame_history>(path, filename, sintra::message_ring_size,
        storage, authority);
    auto notification = std::make_shared<Frame_notification>(path, filename,
        sintra::message_ring_size, record, authority,
        detail::ring_directory_policy::private_existing_directory, std::function<void()>{}, history);
    sintra::Message_ring_R reader(path, k_frame_prefix, k_frame_writer_id, k_occurrence, true,
        sintra::Ring_reader_policy::forcible, authority);
    reader.install_native_notification(notification);
    reader.start_reading();
    std::vector<std::pair<sintra::sequence_counter_type, sintra::sequence_counter_type>> losses;
    reader.set_loss_handler([&](auto first, auto end) { losses.emplace_back(first, end); });
    signal_file(directory / "capabilities_ready");
    wait_for_file(directory / "first_committed");
    check_frame(reader.fetch_message(), 0, k_frame_bytes);
    require(reader.reading_sequence() == frame_start(test, k_initial_frames) &&
        reader.get_message_reading_sequence() == k_frame_bytes,
        "message extraction did not retain its finer cursor within the returned batch");
    signal_file(directory / "readers_ready");
    const bool marker_death = std::string_view(test.name) == "framed_before_saved_marker" ||
        std::string_view(test.name) == "framed_after_saved_marker" ||
        std::string_view(test.name) == "framed_before_exposure";
    wait_for_file(directory / (marker_death ? "death_seam" : "writer_seam"));
    kill_retaining_authority(writer);

    auto original_storage = map_storage(directory / filename);
    auto* original = static_cast<char*>(original_storage->data());
    if (std::string_view(test.name) == "framed_after_saved_marker" ||
        std::string_view(test.name) == "framed_before_exposure")
    {
        // The complete saved block must win even when the original differs.
        std::memset(original, 0xb6, 16);
    }
    const std::vector<char> original_before(original, original + sintra::message_ring_size);
    uint64_t damaged_magic = 0;
    std::memcpy(&damaged_magic, original + 2 * k_frame_bytes, sizeof(damaged_magic));
    if (test.publish_small && !batch_wrap(test)) {
        require(damaged_magic != sintra::message_magic,
            "failed larger construction did not damage the surviving frame prefix");
    }
    if (recoverer) {
        require(notification->observe_exit(), "exact framed writer death was not observed");
        signal_file(directory / "recovery_allowed");
        wait_for_file(directory / "death_seam");
        kill_retaining_authority(recoverer);
        recoverer_record->native_exit.store(true);
        require(notification->pending() && !history->ready(),
            "interrupted compact recovery advertised a partial image");
    }
    if (test.replay_at_release) {
        require(notification->observe_exit() && !history->ready(),
            "framed publication race did not begin with pending recovery");
        s_frame_notification = notification.get();
        detail::test_hooks::s_ipc_wakeup_operation = frame_release_hook;
    }
    else {
        require(notification->replay(k_occurrence).state == Frame_notification::Replay_state::COMPLETE,
            "native framed recovery did not complete");
    }

    const auto frame_count = batch_wrap(test) ? 2 * k_batch_count : sintra::message_ring_size / k_frame_bytes;
    const auto expected_head = frame_start(test, frame_count) + (test.publish_small ? k_small_frame_bytes : 0);
    const auto retained_floor = expected_head - sintra::message_ring_size;
    size_t first_frame = 0;
    while (frame_start(test, first_frame) < retained_floor) { ++first_frame; }
    size_t replayed_wrap_frames = 0;
    for (size_t sequence = std::max(size_t(1), first_frame); sequence < frame_count; ++sequence) {
        check_frame(reader.fetch_message(), sequence, frame_size(test, sequence));
        if (frame_start(test, sequence) % sintra::message_ring_size + frame_size(test, sequence) >
            sintra::message_ring_size)
        {
            ++replayed_wrap_frames;
        }
    }
    require(!batch_wrap(test) || replayed_wrap_frames != 0,
        "recovered window did not retain and replay a complete frame straddling the physical ring end");
    if (test.publish_small) {
        check_frame(reader.fetch_message(), frame_count, k_small_frame_bytes);
    }
    detail::test_hooks::s_ipc_wakeup_operation = nullptr;
    if (test.replay_at_release) {
        require(s_frame_release_calls == 1 && s_frame_release_was_pending && s_frame_release_replayed,
            "framed recovery did not publish at the actual managed range-release seam");
    }
    require(history->first() == frame_start(test, first_frame) && history->head() == expected_head,
        "framed history did not exclude exactly the partially displaced frame");
    require(std::equal(original_before.begin(), original_before.end(), original),
        "framed recovery modified the original ring mapping");
    if (test.publish_small) {
        require(losses.size() == 1 && losses[0].first == k_frame_bytes &&
            losses[0].second == frame_start(test, first_frame),
            "framed recovery did not report exactly the lost unread frame interval");
    }
    else {
        require(losses.empty(), "partial uncommitted overwrite lost a committed frame");
        alignas(Frame) std::array<char, k_frame_bytes> first;
        std::memcpy(first.data(), history->pointer(0), first.size());
        check_frame(reinterpret_cast<const sintra::Message_prefix*>(first.data()), 0, k_frame_bytes);
    }
    require(reader.get_message_reading_sequence() == history->head(),
        "framed extraction did not finish at the committed head");
    reader.request_stop();
    reader.done_reading();
    require(writer->observe_exit_retained() == Exact_child_state::exited,
        "message writer native authority was released before reader cleanup");
    return 0;
}

int run_protected_reader(const fs::path& directory, size_t capacity,
    std::shared_ptr<const detail::Native_exit_authority> authority,
    const std::shared_ptr<Notification>& notification, const std::shared_ptr<Exact_child>& writer)
{
    Guard_reader reader(directory.string(), k_ring_name, capacity, 1,
        detail::ring_directory_policy::caller_directory, notification, false,
        sintra::Ring_reader_policy::protected_reader);
    check_suffix(reader.start_reading(), 0, 1);
    require(reader.guard_counts() != 0, "protected reader did not retain its original guard");
    signal_file(directory / "readers_ready");
    wait_for_file(directory / "writer_seam");
    kill_retaining_authority(writer);
    require(notification->replay(k_occurrence).state == Notification::Replay_state::COMPLETE,
        "protected reader recovery did not complete");
    require(reader.guard_counts() != 0, "replay released the guard before the raw release boundary");
    reader.done_reading_new_data();
    require(reader.guard_counts() == 0, "image selection retained an obsolete protected ring guard");
    const auto retained = reader.wait_for_new_data(sintra::Ring_wait_hint::BLOCKING);
    check_suffix(retained, 1, 2);

    // Keep both the reader and its immutable returned range alive while the
    // actual successor reuses every physical octile of the original mapping.
    Writer successor(directory.string(), k_ring_name, capacity,
        detail::ring_directory_policy::caller_directory, false, authority);
    for (size_t sequence = 2; sequence < capacity + 3; ++sequence) {
        successor.write_commit(payload(sequence));
    }
    require(successor.get_leading_sequence() == capacity + 3,
        "successor could not advance while the recovered protected reader remained alive");
    check_suffix(retained, 1, 2);
    require(notification->replay(k_occurrence).state == Notification::Replay_state::REJECTED,
        "old history replay did not reject the live successor's writer ownership");
    reader.request_stop();
    reader.done_reading();
    require(writer->observe_exit_retained() == Exact_child_state::exited,
        "predecessor native authority was consumed before protected reader cleanup");
    return 0;
}

int run_scenario(const std::string& binary, const history_case_t& test, const fs::path& directory)
{
    if (test.framed) {
        return run_frame_scenario(binary, test, directory);
    }
    const auto path = directory.string();
    const auto capacity = sintra::test::pick_ring_elements<uint32_t>();
    auto writer = std::make_shared<Exact_child>(std::chrono::seconds(3));
    const char* writer_args[] = {binary.c_str(), "--writer", test.name, path.c_str(), nullptr};
    require(writer->spawn(binary.c_str(), writer_args), "cannot spawn exact writer");
    wait_for_file(directory / "writer_armed");
    const auto writer_instance = captured_instance(directory, "writer", *writer);
    std::vector<std::shared_ptr<const detail::Native_exit_witness>> witnesses{
        std::make_shared<Armed_child_witness>(writer_instance, writer)};
    std::shared_ptr<Exact_child> recoverer;
    std::shared_ptr<Native_record> recoverer_record;
    if (test.kill_recoverer) {
        recoverer = std::make_shared<Exact_child>(std::chrono::seconds(3));
        const char* args[] = {binary.c_str(), "--recoverer", test.name, path.c_str(), nullptr};
        require(recoverer->spawn(binary.c_str(), args), "cannot spawn exact recoverer");
        wait_for_file(directory / "recoverer_armed");
        const auto instance = captured_instance(directory, "recoverer", *recoverer);
        auto storage = create_storage(directory / "recoverer_record", sizeof(Native_record));
        recoverer_record = std::shared_ptr<Native_record>(storage,
            new (storage->data()) Native_record(2, instance));
        witnesses.push_back(std::make_shared<Armed_child_witness>(instance, recoverer));
    }
    auto authority = std::make_shared<detail::Native_exit_authority>(std::move(witnesses));
    auto record_storage = create_storage(directory / "writer_record", sizeof(Native_record));
    auto record = std::shared_ptr<Native_record>(record_storage,
        new (record_storage->data()) Native_record(k_occurrence, writer_instance));
    const auto history_bytes = History::storage_size(capacity);
    auto history_storage = create_storage(directory / "history", history_bytes);
    History::initialize(history_storage->data(), history_bytes, capacity, k_occurrence, writer_instance);
    auto history = std::make_shared<History>(path, k_ring_name, capacity, history_storage, authority);
    auto notification = std::make_shared<Notification>(path, k_ring_name, capacity, record, authority,
        detail::ring_directory_policy::caller_directory, std::function<void()>{}, history);
    signal_file(directory / "capabilities_ready");
    wait_for_file(directory / "first_committed");
    if (std::string_view(test.name) == "protected_guard") {
        return run_protected_reader(directory, capacity, authority, notification, writer);
    }

    Reader holder(path, k_ring_name, capacity, 1,
        detail::ring_directory_policy::caller_directory, notification);
    Reader other(path, k_ring_name, capacity, 0,
        detail::ring_directory_policy::caller_directory, notification);
    const auto held_range = holder.start_reading();
    check_suffix(held_range, 0, 1);
    const auto empty = other.start_reading();
    require(empty.begin == empty.end && holder.reading_sequence() == 1 &&
        other.reading_sequence() == 1, "readers did not capture delivered-range sequence one");
    signal_file(directory / "readers_ready");
    wait_for_file(directory / (test.kill_at_head ? "death_seam" : "writer_seam"));
    kill_retaining_authority(writer);

    const auto head = capacity + (test.publish_small ? 1 : 0);
    require(holder.get_leading_sequence() == head, "writer died on the wrong side of head publication");
    auto original_storage = map_storage(directory / k_ring_name);
    const auto* original = static_cast<const uint32_t*>(original_storage->data());
    const std::vector<uint32_t> original_before(original, original + capacity);
    require(original_before[1] == k_poison, "failed larger placement did not damage retained bytes");
    require(notification->observe_exit(), "exact writer death was not observed");
    if (recoverer) {
        signal_file(directory / "recovery_allowed");
        wait_for_file(directory / "death_seam");
        kill_retaining_authority(recoverer);
        recoverer_record->native_exit.store(true);
        require(notification->pending(), "interrupted image recovery lost replay obligation");
    }
    require(notification->replay(k_occurrence).state == Notification::Replay_state::COMPLETE,
        "native recovery did not complete");
    require(std::equal(original_before.begin(), original_before.end(), original),
        "recovery mutated the original ring mapping while a raw caller held its range");

    // The other reader switches while the holder still owns the old raw range.
    // That range may already be overwritten by the writer; never dereference it.
    other.done_reading_new_data();
    check_suffix(other.wait_for_new_data(sintra::Ring_wait_hint::BLOCKING), 1, head);
    require(holder.reading_sequence() == 1, "recovery advanced a held range's delivery cursor");
    require(std::equal(original_before.begin(), original_before.end(), original),
        "switching another reader changed the retained original mapping");
    holder.done_reading_new_data();
    check_suffix(holder.wait_for_new_data(sintra::Ring_wait_hint::BLOCKING), 1, head);
    require(holder.reading_sequence() == head && other.reading_sequence() == head,
        "recovered readers did not finish at the committed head");
    holder.request_stop();
    other.request_stop();
    holder.done_reading();
    other.done_reading();
    require(writer->observe_exit_retained() == Exact_child_state::exited &&
        (!recoverer || recoverer->observe_exit_retained() == Exact_child_state::exited),
        "native child authority was consumed before reader cleanup");
    return 0;
}

bool supervise(const std::string& binary, const history_case_t& test)
{
    sintra::test::Temp_ring_dir directory(std::string("history_") + test.name);
    const auto path = directory.str();
    Exact_child scenario(std::chrono::seconds(3));
    const char* args[] = {binary.c_str(), "--scenario", test.name, path.c_str(), nullptr};
    require(scenario.spawn(binary.c_str(), args), "cannot spawn retained history scenario");
    const bool exited = await([&] { return scenario.poll() != Exact_child_state::running; });
    const bool passed = exited && scenario.exited_with_code(0);
    if (!passed) {
        std::fprintf(stderr, "%s: %s\n", test.name,
            exited ? scenario.describe_status().c_str() : "supervisor deadline: no progress");
    }
    std::string diagnostic;
    require(scenario.terminate_and_settle(diagnostic), diagnostic);
    std::printf("%s: %s\n", test.name, passed ? "PASS" : "FAIL");
    std::fflush(stdout);
    return passed;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto binary = sintra::test::get_binary_path(argc, argv);
        if (argc == 4 && std::string_view(argv[1]) == "--writer") {
            return run_writer(find_case(argv[2]), argv[3]);
        }
        if (argc == 4 && std::string_view(argv[1]) == "--recoverer") {
            return run_recoverer(find_case(argv[2]), argv[3]);
        }
        if (argc == 4 && std::string_view(argv[1]) == "--scenario") {
            return run_scenario(binary, find_case(argv[2]), argv[3]);
        }
        if (argc == 3 && std::string_view(argv[1]) == "--case") {
            return supervise(binary, find_case(argv[2])) ? 0 : 1;
        }
        require(argc == 1, "expected --case NAME or no arguments");
        bool passed = true;
        for (const auto& test : k_cases) {
            passed = supervise(binary, test) && passed;
        }
        return passed ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "ring_retained_history_test: %s\n", error.what());
        return 1;
    }
}
