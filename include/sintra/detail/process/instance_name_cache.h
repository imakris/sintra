#pragma once

#include "../id_types.h"
#include "../ipc/spinlock.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace sintra::detail {

// Remote name snapshots are invalidated by coordinator notifications. Keep
// them separate from the coordinator's authoritative publication registry.
class Instance_name_cache
{
public:
    template <typename Resolve>
    instance_id_type resolve(const std::string& name, Resolve&& authoritative_resolve)
    {
        uint64_t revision;
        {
            spinlock::locker lock(m_lock);
            const auto entry = m_names.find(name);
            if (entry != m_names.end()) {
                return entry->second;
            }
            revision = m_revision;
        }

        const auto resolved = authoritative_resolve(name);
        {
            spinlock::locker lock(m_lock);
            if (revision == m_revision) {
                if (resolved != invalid_instance_id) {
                    m_names.emplace(name, resolved);
                }
                return resolved;
            }
        }

        // Resolve after the observed invalidation, without refilling from a
        // response that could be overtaken again. One retry bounds churn and
        // requires no publication-reader or delivery-fence progress.
        return authoritative_resolve(name);
    }

    void invalidate_name(const std::string& name)
    {
        spinlock::locker lock(m_lock);
        ++m_revision;
        m_names.erase(name);
    }

    void invalidate_instance(const std::string& name, instance_id_type instance)
    {
        spinlock::locker lock(m_lock);
        ++m_revision;
        if (instance == process_of(instance)) {
            for (auto entry = m_names.begin(); entry != m_names.end();) {
                if (process_of(entry->second) == instance) {
                    entry = m_names.erase(entry);
                }
                else {
                    ++entry;
                }
            }
        }
        else {
            const auto entry = m_names.find(name);
            if (entry != m_names.end() && entry->second == instance) {
                m_names.erase(entry);
            }
        }
    }

    void clear()
    {
        spinlock::locker lock(m_lock);
        ++m_revision;
        m_names.clear();
    }

private:
    spinlock m_lock;
    std::unordered_map<std::string, instance_id_type> m_names;
    uint64_t m_revision = 0;
};

} // namespace sintra::detail
