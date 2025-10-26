#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace sintra {

template <typename... Args>
struct message_args;

template <>
struct message_args<>
{
    constexpr message_args() = default;
};

template <typename Head, typename... Tail>
struct message_args<Head, Tail...>
{
    Head head;
    message_args<Tail...> tail;

    constexpr message_args() = default;

    template <typename H, typename... U,
              typename = typename std::enable_if<(sizeof...(U) == sizeof...(Tail))>::type>
    constexpr message_args(H&& h, U&&... u)
        : head(std::forward<H>(h))
        , tail(std::forward<U>(u)...) {}
};

namespace detail {

template <typename...>
using void_t = void;

template <typename T, typename = void>
struct message_args_base_impl;

template <typename... Args>
struct message_args_base_impl<message_args<Args...>, void>
{
    using type = message_args<Args...>;
};

template <typename T>
struct message_args_base_impl<T, void_t<typename T::base_type>>
{
    using type = typename message_args_base_impl<typename T::base_type>::type;
};

template <typename T>
struct message_args_base_impl<T, void>
{
    static_assert(!std::is_same<T, T>::value,
        "message_args_base requires a type derived from message_args");
};

template <typename T>
struct message_args_base
{
    using U = typename std::remove_cv<typename std::remove_reference<T>::type>::type;
    using type = typename message_args_base_impl<U>::type;
};

template <typename T, std::size_t I>
struct message_args_element_impl;

template <typename Head, typename... Tail>
struct message_args_element_impl<message_args<Head, Tail...>, 0>
{
    using type = Head;
};

template <typename Head, typename... Tail, std::size_t I>
struct message_args_element_impl<message_args<Head, Tail...>, I>
{
    static_assert(I < sizeof...(Tail) + 1, "message_args index out of bounds");
    using type = typename message_args_element_impl<message_args<Tail...>, I - 1>::type;
};

template <std::size_t I>
struct message_args_element_impl<message_args<>, I>
{
    static_assert(I == 0, "message_args index out of bounds");
};

} // namespace detail

template <typename T>
struct message_args_size
    : message_args_size<typename detail::message_args_base<T>::type>
{};

template <typename... Args>
struct message_args_size<message_args<Args...>>
    : std::integral_constant<std::size_t, sizeof...(Args)>
{};

template <typename T, std::size_t I>
struct message_args_element
{
    using base_type = typename detail::message_args_base<T>::type;
    using type = typename detail::message_args_element_impl<base_type, I>::type;
};

namespace detail {

template <std::size_t I>
struct message_args_get
{
    template <typename Head, typename... Tail>
    static auto& apply(message_args<Head, Tail...>& args)
    {
        return message_args_get<I - 1>::apply(args.tail);
    }

    template <typename Head, typename... Tail>
    static const auto& apply(const message_args<Head, Tail...>& args)
    {
        return message_args_get<I - 1>::apply(args.tail);
    }

    template <typename Head, typename... Tail>
    static auto&& apply(message_args<Head, Tail...>&& args)
    {
        return message_args_get<I - 1>::apply(std::move(args.tail));
    }
};

template <>
struct message_args_get<0>
{
    template <typename Head, typename... Tail>
    static Head& apply(message_args<Head, Tail...>& args)
    {
        return args.head;
    }

    template <typename Head, typename... Tail>
    static const Head& apply(const message_args<Head, Tail...>& args)
    {
        return args.head;
    }

    template <typename Head, typename... Tail>
    static Head&& apply(message_args<Head, Tail...>&& args)
    {
        return std::move(args.head);
    }
};

} // namespace detail

template <std::size_t I, typename T>
auto& get(T& args)
{
    using base_type = typename detail::message_args_base<T>::type;
    return detail::message_args_get<I>::apply(static_cast<base_type&>(args));
}

template <std::size_t I, typename T>
const auto& get(const T& args)
{
    using base_type = typename detail::message_args_base<T>::type;
    return detail::message_args_get<I>::apply(static_cast<const base_type&>(args));
}

template <std::size_t I, typename T>
auto&& get(T&& args)
{
    using base_type = typename detail::message_args_base<T>::type;
    return detail::message_args_get<I>::apply(static_cast<base_type&&>(args));
}

} // namespace sintra

