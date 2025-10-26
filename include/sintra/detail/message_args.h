// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace sintra {

namespace detail {

template <typename>
struct always_false: std::false_type {};

} // namespace detail

template <typename... Args>
struct message_args;

template <>
struct message_args<>
{
    using message_args_base = message_args<>;
};

template <typename Head>
struct message_args<Head>
{
    using message_args_base = message_args<Head>;

    Head value;
};

template <typename Head, typename Next, typename... Tail>
struct message_args<Head, Next, Tail...>
{
    using message_args_base = message_args<Head, Next, Tail...>;

    Head value;
    message_args<Next, Tail...> rest;
};


template <typename T, typename = void>
struct message_args_size;

template <typename... Args>
struct message_args_size<message_args<Args...>, void>
    : std::integral_constant<std::size_t, sizeof...(Args)>
{};

template <typename T>
struct message_args_size<T, std::void_t<typename T::message_args_base>>
    : message_args_size<typename T::message_args_base>
{};


template <typename T, std::size_t I, typename = void>
struct message_args_element;

template <typename... Args, std::size_t I>
struct message_args_element<message_args<Args...>, I, void>
{
    static_assert(I < sizeof...(Args), "Index out of bounds in message_args_element");
    using type = typename std::tuple_element<I, std::tuple<Args...>>::type;
};

template <typename T, std::size_t I>
struct message_args_element<T, I, std::void_t<typename T::message_args_base>>
    : message_args_element<typename T::message_args_base, I>
{};


template <std::size_t I, typename Head, typename... Tail>
constexpr auto& get(message_args<Head, Tail...>& seq) noexcept
{
    if constexpr (I == 0) {
        return seq.value;
    }
    else {
        if constexpr (sizeof...(Tail) == 0) {
            static_assert(detail::always_false<std::integral_constant<std::size_t, I>>::value,
                "Index out of bounds in message_args::get");
            return seq.value; // Unreachable, satisfies return type.
        }
        else {
            return get<I - 1>(seq.rest);
        }
    }
}

template <std::size_t I, typename Head, typename... Tail>
constexpr const auto& get(const message_args<Head, Tail...>& seq) noexcept
{
    if constexpr (I == 0) {
        return seq.value;
    }
    else {
        if constexpr (sizeof...(Tail) == 0) {
            static_assert(detail::always_false<std::integral_constant<std::size_t, I>>::value,
                "Index out of bounds in message_args::get");
            return seq.value; // Unreachable, satisfies return type.
        }
        else {
            return get<I - 1>(seq.rest);
        }
    }
}

template <std::size_t I, typename Head, typename... Tail>
constexpr auto&& get(message_args<Head, Tail...>&& seq) noexcept
{
    return std::move(get<I>(seq));
}

template <std::size_t I, typename Head, typename... Tail>
constexpr const auto&& get(const message_args<Head, Tail...>&& seq) noexcept
{
    return std::move(get<I>(seq));
}
} // namespace sintra

