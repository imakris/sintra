#include <sintra/sintra.h>

#include "test_utils.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

constexpr const char* k_peer_name = "name_cache_peer";
constexpr const char* k_subject_name = "name_cache_subject";

bool check(bool passed, const char* message)
{
    return sintra::test::assert_true(passed, "instance_name_cache_test: ", message);
}

bool check_cache_ordering()
{
    using sintra::detail::Instance_name_cache;
    constexpr auto old_id = sintra::compose_instance(4, 11);
    constexpr auto new_id = sintra::compose_instance(4, 12);
    constexpr auto other_id = sintra::compose_instance(5, 11);
    bool passed = true;

    Instance_name_cache cache;
    unsigned calls = 0;
    auto resolve_new = [&](const std::string&) { ++calls; return new_id; };
    passed &= check(cache.resolve("sensor", resolve_new) == new_id &&
        cache.resolve("sensor", resolve_new) == new_id && calls == 1,
        "repeated cache hits must not invoke the coordinator");
    cache.invalidate_instance("sensor", old_id);
    passed &= check(cache.resolve("sensor", resolve_new) == new_id && calls == 1,
        "late retirement of an older instance must preserve its replacement");

    cache.resolve("second", [](const std::string&) { return old_id; });
    cache.resolve("other-process", [](const std::string&) { return other_id; });
    cache.invalidate_instance("process", sintra::process_of(old_id));
    unsigned invalid_calls = 0;
    auto absent = [&](const std::string&) { ++invalid_calls; return sintra::invalid_instance_id; };
    passed &= check(cache.resolve("sensor", absent) == sintra::invalid_instance_id &&
        cache.resolve("second", absent) == sintra::invalid_instance_id &&
        cache.resolve("other-process", absent) == other_id && invalid_calls == 2,
        "process retirement must erase its names and preserve other processes");

    // The fake transport retires the captured result before completing the
    // first reply. It deterministically models a reply overtaken by a request
    // notification, including the case where no cache entry existed to erase.
    Instance_name_cache racing;
    unsigned racing_calls = 0;
    auto overtaken = [&](const std::string& name) {
        if (++racing_calls == 1) {
            racing.invalidate_instance(name, old_id);
            return old_id;
        }
        return new_id;
    };
    passed &= check(racing.resolve("race", overtaken) == new_id && racing_calls == 2,
        "an invalidated reply must be discarded for the caller as well as the cache");
    passed &= check(racing.resolve("race", overtaken) == new_id && racing_calls == 3 &&
        racing.resolve("race", overtaken) == new_id && racing_calls == 3,
        "the bounded retry must stay uncached; a later stable result can be cached");

    Instance_name_cache published;
    unsigned publication_calls = 0;
    auto became_available = [&](const std::string& name) {
        if (++publication_calls == 1) {
            published.invalidate_name(name);
            return sintra::invalid_instance_id;
        }
        return new_id;
    };
    passed &= check(published.resolve("new", became_available) == new_id && publication_calls == 2,
        "publication overtaking an absent reply must trigger fresh resolution");

    Instance_name_cache lossy;
    unsigned loss_calls = 0;
    auto repeated_loss = [&](const std::string&) {
        lossy.clear();
        return ++loss_calls == 1 ? old_id : new_id;
    };
    passed &= check(lossy.resolve("lost", repeated_loss) == new_id && loss_calls == 2,
        "repeated invalidation must not create a retry loop or return the first stale reply");
    passed &= check(lossy.resolve("lost", absent) == sintra::invalid_instance_id && invalid_calls == 3,
        "a retry overtaken again must not refill the cache");
    return passed;
}

class Cache_peer : public sintra::Derived_transceiver<Cache_peer>
{
public:
    ~Cache_peer() { destroy(); }

    sintra::instance_id_type lookup(const std::string& name)
    {
        return sintra::get_instance_id(std::string(name));
    }

    SINTRA_RPC(lookup)
};

int client()
{
    Cache_peer peer;
    peer.assign_name(k_peer_name);
    sintra::barrier("name-cache-ready", "_sintra_all_processes");
    sintra::barrier("name-cache-finished", "_sintra_all_processes");
    return 0;
}

bool check_name_reuse()
{
    using namespace std::chrono_literals;
    const auto peer = sintra::get_instance_id(std::string(k_peer_name));
    auto lookup = [&]() {
        return Cache_peer::rpc_async_lookup(peer, k_subject_name).get_until(
            std::chrono::steady_clock::now() + 3s);
    };
    sintra::Transceiver original(k_subject_name);
    const auto original_id = original.instance_id();
    bool passed = sintra::test::assert_true(
        lookup() == original_id && lookup() == original_id,
        "instance_name_cache_test: ", "initial remote lookup must resolve the live publication");
    original.destroy();

    // Unpublication and this RPC share the coordinator request FIFO, so the
    // peer observes retirement before looking up the formerly cached name.
    passed &= sintra::test::assert_true(lookup() == sintra::invalid_instance_id,
        "instance_name_cache_test: ", "individual unpublication must invalidate a cached name");
    sintra::Transceiver replacement(k_subject_name);
    passed &= sintra::test::assert_true(lookup() == replacement.instance_id(),
        "instance_name_cache_test: ", "same-name replacement must resolve to its new instance");
    return passed;
}

bool check_coordinator_authority()
{
    constexpr const char* name = "name_cache_authority";
    constexpr auto stale = sintra::compose_instance(4, 19);
    sintra::s_mproc->m_instance_name_cache.resolve(name,
        [](const std::string&) { return stale; });
    bool passed = check(sintra::get_instance_id(std::string(name)) == sintra::invalid_instance_id,
        "a remote snapshot must never introduce a coordinator publication");
    sintra::Transceiver object(name);
    passed &= check(sintra::get_instance_id(std::string(name)) == object.instance_id(),
        "coordinator lookup must read its authoritative publication");
    object.destroy();
    passed &= check(sintra::get_instance_id(std::string(name)) == sintra::invalid_instance_id,
        "coordinator lookup must not resurrect a retired publication");
    return passed;
}

} // namespace

int main(int argc, char* argv[])
{
    return sintra::test::run_multi_process_test(
        argc, argv, "SINTRA_NAME_CACHE_DIR", "name_cache", {client},
        [](const std::filesystem::path&) {
            sintra::barrier("name-cache-ready", "_sintra_all_processes");
            bool passed = check_cache_ordering();
            passed &= check_coordinator_authority();
            passed &= check_name_reuse();
            sintra::barrier("name-cache-finished", "_sintra_all_processes");
            return passed ? 0 : 1;
        },
        [](const std::filesystem::path&) { return 0; });
}
