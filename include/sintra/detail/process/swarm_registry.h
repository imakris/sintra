// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "../id_types.h"
#include "../ipc/file_mapping.h"
#include "../ipc/mutex.h"
#include "../ipc/platform_utils.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

namespace sintra::detail {

class Swarm_registry
{
public:
    struct Registry_layout {
        std::atomic<uint64_t> id_bitmap[4];
        interprocess_mutex    lobby_mutex;
    };

    explicit Swarm_registry(const std::string& swarm_directory)
    {
        namespace fs = std::filesystem;
        const fs::path registry_path = fs::path(swarm_directory) / "sintra_registry.bin";
        const bool valid_size = fs::exists(registry_path) &&
                                fs::is_regular_file(registry_path) &&
                                fs::file_size(registry_path) == sizeof(Registry_layout);

        if (!valid_size) {
            create_registry_file(registry_path);
        }

        ipc::file_mapping mapping(registry_path, ipc::read_write);
        m_region = std::make_unique<ipc::mapped_region>(mapping, ipc::read_write, 0, 0);
        m_layout = static_cast<Registry_layout*>(m_region->data());

        if (!valid_size) {
            std::memset(m_layout, 0, sizeof(Registry_layout));
            m_layout->id_bitmap[0].store(0x3ull, std::memory_order_relaxed); // reserve indices 0 and 1
        }
    }

    instance_id_type allocate_process_id()
    {
        if (!m_layout) {
            throw std::runtime_error("Swarm_registry: registry not mapped");
        }

        constexpr uint32_t max_process = static_cast<uint32_t>(max_process_index);

        for (size_t word_index = 0; word_index < std::size(m_layout->id_bitmap); ++word_index) {
            auto& word = m_layout->id_bitmap[word_index];
            uint64_t current = word.load(std::memory_order_acquire);

            while (true) {
                uint64_t available = ~current;
                if (word_index == 0) {
                    available &= ~uint64_t(0x3); // skip indices 0 and 1
                }

                if (available == 0) {
                    break;
                }

                unsigned bit = 0;
                while (bit < 64 && ((available & (uint64_t(1) << bit)) == 0)) {
                    ++bit;
                }

                if (bit >= 64) {
                    break;
                }

                const uint32_t process_index = static_cast<uint32_t>(word_index * 64 + bit);
                if (process_index == 0 || process_index == 1 || process_index > max_process) {
                    current |= (uint64_t(1) << bit);
                    continue;
                }

                const uint64_t desired = current | (uint64_t(1) << bit);
                if (word.compare_exchange_weak(
                        current, desired, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    return make_process_instance_id(process_index);
                }
            }
        }

        throw std::runtime_error("Swarm_registry is full");
    }

    void free_process_id(instance_id_type id)
    {
        if (!m_layout) {
            return;
        }

        const auto process_index = static_cast<uint32_t>(get_process_index(id));
        if (process_index == 0 || process_index == 1 || process_index > static_cast<uint32_t>(max_process_index)) {
            return;
        }

        const size_t word_index = process_index / 64;
        const unsigned bit = static_cast<unsigned>(process_index % 64);
        const uint64_t mask = ~(uint64_t(1) << bit);

        m_layout->id_bitmap[word_index].fetch_and(mask, std::memory_order_acq_rel);
    }

    void reserve_process_id(instance_id_type id)
    {
        if (!m_layout) {
            return;
        }

        const auto process_index = static_cast<uint32_t>(get_process_index(id));
        if (process_index == 0 || process_index == 1 || process_index > static_cast<uint32_t>(max_process_index)) {
            return;
        }

        const size_t word_index = process_index / 64;
        const unsigned bit = static_cast<unsigned>(process_index % 64);
        const uint64_t mask = (uint64_t(1) << bit);
        m_layout->id_bitmap[word_index].fetch_or(mask, std::memory_order_acq_rel);
    }

    void lock_lobby()
    {
        if (m_layout) {
            m_layout->lobby_mutex.lock();
        }
    }

    void unlock_lobby()
    {
        if (m_layout) {
            m_layout->lobby_mutex.unlock();
        }
    }

private:
    static void create_registry_file(const std::filesystem::path& path)
    {
        const auto native = path.string();
        auto handle = create_new_file(native.c_str());
        if (handle == invalid_file()) {
            throw std::runtime_error("Swarm_registry: failed to create registry file");
        }

        const bool truncated = truncate_file(handle, sizeof(Registry_layout));
        const bool closed = close_file(handle);
        if (!truncated || !closed) {
            throw std::runtime_error("Swarm_registry: failed to initialize registry file");
        }
    }

    std::unique_ptr<ipc::mapped_region> m_region;
    Registry_layout*                    m_layout = nullptr;
};

} // namespace sintra::detail
