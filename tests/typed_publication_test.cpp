#include <sintra/sintra.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

class Explicit_bus : public sintra::Derived_transceiver<Explicit_bus>
{
public:
    using Derived_transceiver::Derived_transceiver;
    ~Explicit_bus() { destroy(); }

    SINTRA_TYPE_ID(0x73c02)
    SINTRA_MESSAGE(Notice, int value);
};

class Dynamic_bus : public sintra::Derived_transceiver<Dynamic_bus>
{
public:
    using Derived_transceiver::Derived_transceiver;
    ~Dynamic_bus() { destroy(); }

    SINTRA_MESSAGE(Notice, int value);
};

class Child_bus : public sintra::Derived_transceiver<Child_bus, Dynamic_bus>
{
public:
    ~Child_bus() { destroy(); }
    SINTRA_TYPE_ID(0x73c03)
};

template <typename Bus, typename Construct>
bool check_publication(const std::string& name, Construct&& construct)
{
    std::mutex mutex;
    std::condition_variable changed;
    sintra::type_id_type published_type = sintra::invalid_type_id;
    sintra::instance_id_type published_instance = sintra::invalid_instance_id;
    bool delivered = false;
    sintra::Transceiver receiver;
    receiver.activate([&](const sintra::Coordinator::instance_published& message) {
        if (static_cast<std::string>(message.assigned_name) == name) {
            std::lock_guard lock(mutex);
            published_type = message.type_id;
            published_instance = message.instance_id;
            changed.notify_all();
        }
    }, sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));
    receiver.activate([&](const typename Bus::Notice& message) {
        std::lock_guard lock(mutex);
        delivered = message.value == 17;
        changed.notify_all();
    }, Bus::named_instance(name));

    auto bus = construct();
    // The sender is fully constructed before touching it or emitting. The
    // earlier publication promises type identity, not most-derived readiness.
    bus->template emit_global<typename Bus::Notice>(17);
    std::unique_lock lock(mutex);
    const bool completed = changed.wait_for(lock, std::chrono::seconds(3), [&]() {
        return published_instance != sintra::invalid_instance_id && delivered;
    });
    const auto observed_type = published_type;
    const bool observed_delivery = delivered;
    const bool correct_type = observed_type == sintra::get_type_id<Bus>();
    const bool correct_instance = published_instance == bus->instance_id();
    lock.unlock();
    if (!completed || !correct_type || !correct_instance) {
        std::fprintf(stderr,
            "typed_publication_test: %s published_type=%llu expected_type=%llu instance=%d deferred_delivery=%d\n",
            name.c_str(), static_cast<unsigned long long>(observed_type),
            static_cast<unsigned long long>(sintra::get_type_id<Bus>()), correct_instance, observed_delivery);
        return false;
    }
    return true;
}

bool run_checks()
{
    const std::string string_name = "typed-string-constructor";
    bool passed = check_publication<Explicit_bus>(string_name, [&]() {
        return std::make_unique<Explicit_bus>(string_name);
    });
    constexpr const char* literal_name = "typed-char-constructor";
    const auto supplied_id = sintra::make_instance_id();
    passed &= check_publication<Explicit_bus>(literal_name, [&]() {
        auto object = std::make_unique<Explicit_bus>(literal_name, supplied_id);
        if (object->instance_id() != supplied_id) {
            throw std::runtime_error("constructor ignored the supplied instance ID");
        }
        return object;
    });
    constexpr const char* dynamic_name = "typed-dynamic-constructor";
    passed &= check_publication<Dynamic_bus>(dynamic_name, [&]() {
        return std::make_unique<Dynamic_bus>(dynamic_name);
    });

    // Higher-level CRTP and explicit post-construction publication retain their
    // existing path; an application can finish its state setup before naming.
    constexpr const char* child_name = "typed-child-delayed-publication";
    passed &= check_publication<Child_bus>(child_name, [&]() {
        auto object = std::make_unique<Child_bus>();
        if (!object->assign_name(child_name)) {
            throw std::runtime_error("post-construction name assignment failed");
        }
        return object;
    });
    return passed;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);
    bool passed = false;
    try {
        passed = run_checks();
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "typed_publication_test: %s\n", error.what());
    }
    sintra::shutdown();
    return passed ? 0 : 1;
}
