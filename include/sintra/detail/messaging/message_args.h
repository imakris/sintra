#pragma once

#include <cstddef>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace sintra {
namespace detail {

template <typename T>
using message_arg_storage_value_t = std::conditional_t<
    std::is_reference<T>::value,
    std::reference_wrapper<typename std::remove_reference<T>::type>,
    T>;

template <std::size_t I, typename T>
struct message_arg_storage {
    using storage_type = message_arg_storage_value_t<T>;

    storage_type value;

    template <typename U = storage_type,
              typename = std::enable_if_t<std::is_default_constructible<U>::value>>
    constexpr message_arg_storage() noexcept(std::is_nothrow_default_constructible<U>::value)
        : value()
    {
    }

    template <typename U,
              typename = std::enable_if_t<std::is_constructible<storage_type, U&&>::value>>
    constexpr message_arg_storage(U&& v) noexcept(
        std::is_nothrow_constructible<storage_type, U&&>::value)
        : value(std::forward<U>(v))
    {
    }

    template <typename U>
    static constexpr decltype(auto) access(U&& v) noexcept
    {
        if constexpr (std::is_reference<T>::value) {
            return v.get();
        }
        else {
            return std::forward<U>(v);
        }
    }
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
struct is_message_args : std::false_type {};

template <typename... Args>
struct is_message_args<message_args<Args...>> : std::true_type {};

template <typename T>
using message_args_decay_t =
    typename std::remove_cv<typename std::remove_reference<T>::type>::type;

template <typename T>
struct message_args_size
    : message_args_size<typename message_args_decay_t<T>::message_args_type>
{};

template <typename... Args>
struct message_args_size<message_args<Args...>>
    : std::integral_constant<std::size_t, sizeof...(Args)>
{};

template <typename T, std::size_t I>
struct message_args_nth_type
    : message_args_nth_type<typename message_args_decay_t<T>::message_args_type, I>
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

template <typename StorageT>
constexpr decltype(auto) access_message_arg(StorageT&& storage) noexcept
{
    using storage_type = std::remove_reference_t<StorageT>;
    return storage_type::access(std::forward<StorageT>(storage).value);
}

template <std::size_t I,
          typename ArgsT,
          typename = std::enable_if_t<is_message_args<message_args_decay_t<ArgsT>>::value>>
constexpr decltype(auto) get(ArgsT&& args) noexcept
{
    using args_decay_t = message_args_decay_t<ArgsT>;
    using storage_t = message_arg_storage<I, typename message_args_nth_type<args_decay_t, I>::type>;

    if constexpr (std::is_lvalue_reference<ArgsT&&>::value) {
        if constexpr (std::is_const<std::remove_reference_t<ArgsT>>::value) {
            return access_message_arg(static_cast<const storage_t&>(args));
        }
        else {
            return access_message_arg(static_cast<storage_t&>(args));
        }
    }
    else {
        if constexpr (std::is_const<std::remove_reference_t<ArgsT>>::value) {
            return access_message_arg(static_cast<const storage_t&&>(std::move(args)));
        }
        else {
            return access_message_arg(static_cast<storage_t&&>(std::move(args)));
        }
    }
}

} // namespace detail
} // namespace sintra

