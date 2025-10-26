// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace sintra::detail
{

template <std::size_t I, typename T>
struct message_arg_holder
{
    T value;

    message_arg_holder() = default;

    template <typename U>
    constexpr explicit message_arg_holder(U&& v)
        : value(std::forward<U>(v))
    {}
};

template <std::size_t I, typename... Args>
struct message_args_element;

template <typename T, typename... Args>
struct message_args_element<0, T, Args...>
{
    using type = T;
};

template <std::size_t I, typename T, typename... Args>
struct message_args_element<I, T, Args...> : message_args_element<I - 1, Args...>
{};

template <std::size_t I>
struct message_args_element<I>
{
    static_assert(I == 0, "message_args index out of range");
};

template <typename IndexSequence, typename... Args>
struct message_args_base;

template <std::size_t... I, typename... Args>
struct message_args_base<std::index_sequence<I...>, Args...>
    : message_arg_holder<I, Args>...
{
    message_args_base() = default;

    template <typename... U,
        typename = std::enable_if_t<sizeof...(U) == sizeof...(Args)>>
    constexpr explicit message_args_base(U&&... u)
        : message_arg_holder<I, Args>(std::forward<U>(u))...
    {}
};

template <typename... Args>
struct message_args : message_args_base<std::index_sequence_for<Args...>, Args...>
{
    using base = message_args_base<std::index_sequence_for<Args...>, Args...>;
    using base::base;

    static constexpr std::size_t size = sizeof...(Args);
    using message_args_base_type = message_args<Args...>;

    template <std::size_t I>
    constexpr auto& element() & noexcept
    {
        using holder = message_arg_holder<I, typename message_args_element<I, Args...>::type>;
        return static_cast<holder&>(*this).value;
    }

    template <std::size_t I>
    constexpr const auto& element() const& noexcept
    {
        using holder = message_arg_holder<I, typename message_args_element<I, Args...>::type>;
        return static_cast<const holder&>(*this).value;
    }

    template <std::size_t I>
    constexpr auto&& element() && noexcept
    {
        using holder = message_arg_holder<I, typename message_args_element<I, Args...>::type>;
        return std::move(static_cast<holder&&>(*this).value);
    }

    template <std::size_t I>
    constexpr const auto&& element() const&& noexcept
    {
        using holder = message_arg_holder<I, typename message_args_element<I, Args...>::type>;
        return std::move(static_cast<const holder&&>(*this).value);
    }
};

template <typename T>
struct message_args_decay
{
    using type = typename std::remove_const_t<
        std::remove_reference_t<T>>::message_args_base_type;
};

template <typename T>
using message_args_decay_t = typename message_args_decay<T>::type;

template <typename T>
struct message_args_size
    : std::integral_constant<std::size_t, message_args_decay_t<T>::size>
{};

template <typename T, std::size_t I>
struct message_args_nth_type
{
    using type = typename message_args_nth_type<message_args_decay_t<T>, I>::type;
};

template <std::size_t I, typename... Args>
struct message_args_nth_type<message_args<Args...>, I>
{
    using type = typename message_args_element<I, Args...>::type;
};

namespace message_args_detail
{

template <std::size_t I, typename... Args>
constexpr auto& get_impl(message_args<Args...>& args) noexcept
{
    return args.template element<I>();
}

template <std::size_t I, typename... Args>
constexpr const auto& get_impl(const message_args<Args...>& args) noexcept
{
    return args.template element<I>();
}

template <std::size_t I, typename... Args>
constexpr auto&& get_impl(message_args<Args...>&& args) noexcept
{
    return std::move(args).template element<I>();
}

template <std::size_t I, typename... Args>
constexpr const auto&& get_impl(const message_args<Args...>&& args) noexcept
{
    return std::move(args).template element<I>();
}

} // namespace message_args_detail

template <std::size_t I, typename T>
constexpr decltype(auto) get(T&& t) noexcept
{
    using base = message_args_decay_t<T>;
    using bare_t = std::remove_reference_t<T>;

    if constexpr (std::is_lvalue_reference_v<T&&>)
    {
        if constexpr (std::is_const_v<bare_t>)
        {
            return message_args_detail::get_impl<I>(static_cast<const base&>(t));
        }
        else
        {
            return message_args_detail::get_impl<I>(static_cast<base&>(t));
        }
    }
    else
    {
        if constexpr (std::is_const_v<bare_t>)
        {
            return message_args_detail::get_impl<I>(static_cast<const base&&>(std::forward<T>(t)));
        }
        else
        {
            return message_args_detail::get_impl<I>(static_cast<base&&>(std::forward<T>(t)));
        }
    }
}

template <typename T>
inline constexpr std::size_t message_args_size_v = message_args_size<T>::value;

} // namespace sintra::detail

