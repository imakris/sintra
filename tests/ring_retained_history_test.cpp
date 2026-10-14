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

void death_hook(const char* stage, const void*)
{
    if (s_death_stage != stage) {
        return;
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

int run_frame_writer(const history_case_t& test, const fs::path& directory)
{
    publish_instance(directory, "writer");
    wait_for_file(directory / "capabilities_ready");
    const auto filename = sintra::get_base_filename(k_frame_prefix, k_frame_writer_id, k_occurrence);
    auto authority = child_authority(directory, false);
    auto history = std::make_shared<Frame_history>(directory.string(), filename,
        sintra::message_ring_size, map_storage(directory / "history"), authority);
    sintra::Message_ring_W writer(directory.string(), k_frame_prefix, k_frame_writer_id,
        k_occurrence, authority, history);
    for (size_t sequence = 0; sequence < k_initial_frames; ++sequence) {
        write_frame(writer, sequence, k_frame_bytes);
    }
    signal_file(directory / "first_committed");
    wait_for_file(directory / "readers_ready");
    const auto frame_count = sintra::message_ring_size / k_frame_bytes;
    for (size_t sequence = k_initial_frames; sequence < frame_count; ++sequence) {
        write_frame(writer, sequence, k_frame_bytes);
    }
    require(writer.get_leading_sequence() == sintra::message_ring_size,
        "typed frames did not fill exactly one ring lap");
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

int run_recoverer(const fs::path& directory)
{
    s_directory = directory;
    publish_instance(directory, "recoverer");
    wait_for_file(directory / "recovery_allowed");
    const auto capacity = sintra::test::pick_ring_elements<uint32_t>();
    auto authority = child_authority(directory, true);
    auto record = map_record(directory, "writer_record");
    auto history = std::make_shared<History>(directory.string(), k_ring_name, capacity,
        map_storage(directory / "history"), authority);
    Notification notification(directory.string(), k_ring_name, capacity, record, authority,
        detail::ring_directory_policy::caller_directory, std::function<void()>{}, history);
    s_death_stage = "history_image_copy";
    detail::test_hooks::s_ipc_wakeup_operation = death_hook;
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
    auto authority = std::make_shared<detail::Native_exit_authority>(
        std::vector<std::shared_ptr<const detail::Native_exit_witness>>{
            std::make_shared<Armed_child_witness>(instance, writer)});
    auto record_storage = create_storage(directory / "writer_record", sizeof(Native_record));
    auto record = std::shared_ptr<Native_record>(record_storage,
        new (record_storage->data()) Native_record(k_occurrence, instance));
    const auto bytes = Frame_history::storage_size(sintra::message_ring_size);
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
    require(reader.reading_sequence() == k_initial_frames * k_frame_bytes &&
        reader.get_message_reading_sequence() == k_frame_bytes,
        "message extraction did not retain its finer cursor within the returned batch");
    signal_file(directory / "readers_ready");
    wait_for_file(directory / "writer_seam");
    kill_retaining_authority(writer);

    auto original_storage = map_storage(directory / filename);
    const auto* original = static_cast<const char*>(original_storage->data());
    const std::vector<char> original_before(original, original + sintra::message_ring_size);
    uint64_t damaged_magic = 0;
    std::memcpy(&damaged_magic, original + 2 * k_frame_bytes, sizeof(damaged_magic));
    require(damaged_magic != sintra::message_magic,
        "failed larger construction did not damage the surviving frame prefix");
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

    const auto frame_count = sintra::message_ring_size / k_frame_bytes;
    for (size_t sequence = 2; sequence < frame_count; ++sequence) {
        check_frame(reader.fetch_message(), sequence, k_frame_bytes);
    }
    check_frame(reader.fetch_message(), frame_count, k_small_frame_bytes);
    detail::test_hooks::s_ipc_wakeup_operation = nullptr;
    if (test.replay_at_release) {
        require(s_frame_release_calls == 1 && s_frame_release_was_pending && s_frame_release_replayed,
            "framed recovery did not publish at the actual managed range-release seam");
    }
    require(history->first() == 2 * k_frame_bytes &&
        history->head() == sintra::message_ring_size + k_small_frame_bytes,
        "framed history did not exclude exactly the partially displaced frame");
    require(std::equal(original_before.begin(), original_before.end(), original),
        "framed recovery modified the original ring mapping");
    require(losses.size() == 1 && losses[0].first == k_frame_bytes &&
        losses[0].second == 2 * k_frame_bytes,
        "framed recovery did not report exactly the lost unread frame interval");
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
        const char* args[] = {binary.c_str(), "--recoverer", path.c_str(), nullptr};
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
        if (argc == 3 && std::string_view(argv[1]) == "--recoverer") {
            return run_recoverer(argv[2]);
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
