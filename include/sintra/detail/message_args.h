#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace sintra {
namespace detail {

template <std::size_t I, typename T>
struct message_arg_storage {
    T value;

    constexpr message_arg_storage() = default;
    constexpr message_arg_storage(const T& v) : value(v) {}
    constexpr message_arg_storage(T&& v) : value(std::move(v)) {}
};

template <typename Seq, typename... Args>
struct message_args_base;

template <std::size_t... I, typename... Args>
struct message_args_base<std::index_sequence<I...>, Args...>
    : message_arg_storage<I, Args>...
{
    constexpr message_args_base() = default;

    template <typename... Ts,
              typename = std::enable_if_t<sizeof...(Ts) == sizeof...(Args)>>
    constexpr message_args_base(Ts&&... ts)
        : message_arg_storage<I, Args>(std::forward<Ts>(ts))...
    {
    }
};

template <typename... Args>
struct message_args
    : message_args_base<std::index_sequence_for<Args...>, Args...>
{
    using base = message_args_base<std::index_sequence_for<Args...>, Args...>;
    using base::base;
    using message_args_type = message_args<Args...>;
};

template <typename T>
struct message_args_size
    : message_args_size<typename std::remove_cv<T>::type::message_args_type>
{};

template <typename... Args>
struct message_args_size<message_args<Args...>>
    : std::integral_constant<std::size_t, sizeof...(Args)>
{};

template <typename T, std::size_t I>
struct message_args_nth_type
    : message_args_nth_type<typename std::remove_cv<T>::type::message_args_type, I>
{};

template <std::size_t I, typename... Args>
struct message_args_nth_type<message_args<Args...>, I>
{
    static_assert(I < sizeof...(Args), "message_args index out of range");
    using type = typename std::tuple_element<I, std::tuple<Args...>>::type;
};

template <std::size_t I, typename... Args>
using message_args_storage_t =
    message_arg_storage<I, typename message_args_nth_type<message_args<Args...>, I>::type>;

template <std::size_t I, typename... Args>
constexpr decltype(auto) get(message_args<Args...>& args) noexcept
{
    return static_cast<message_args_storage_t<I, Args...>&>(args).value;
}

template <std::size_t I, typename... Args>
constexpr decltype(auto) get(const message_args<Args...>& args) noexcept
{
    return static_cast<const message_args_storage_t<I, Args...>&>(args).value;
}

template <std::size_t I, typename... Args>
constexpr decltype(auto) get(message_args<Args...>&& args) noexcept
{
    using storage_t = message_args_storage_t<I, Args...>;
    return std::move(static_cast<storage_t&>(args).value);
}

template <std::size_t I, typename... Args>
constexpr decltype(auto) get(const message_args<Args...>&& args) noexcept
{
    using storage_t = message_args_storage_t<I, Args...>;
    return std::move(static_cast<const storage_t&>(args).value);
}

} // namespace detail
} // namespace sintra

