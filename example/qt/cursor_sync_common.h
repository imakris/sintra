#pragma once
/**
 * @file cursor_sync_common.h
 * @brief Shared message definitions for the Qt cursor sync example.
 */
#include <sintra/sintra.h>

namespace sintra_example {

constexpr const char* k_sender_name = "cursor_sender";
constexpr const char* k_receiver_name = "cursor_receiver";

struct Cursor_sender_bus : sintra::Derived_transceiver<Cursor_sender_bus>
{
    using sintra::Derived_transceiver<Cursor_sender_bus>::Derived_transceiver;

    SINTRA_MESSAGE(cursor_position, int x, int y);
};

} // namespace sintra_example
