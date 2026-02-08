// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <functional>
#include <utility>

#include "../messaging/message_args.h"


namespace sintra {

    
using detail::get;
using detail::message_args_size;


namespace detail {

template <typename F, typename MA, std::size_t... Is>
inline auto call_with_msg_args_impl(
    F&& f,
    const MA& args,
    std::index_sequence<Is...>)
{
    return std::invoke(std::forward<F>(f), get<Is>(args)...);
}

template <typename TObj, typename F, typename MA, std::size_t... Is>
inline auto call_with_msg_args_impl(
    TObj& obj,
    F&& f,
    const MA& args,
    std::index_sequence<Is...>)
{
    return std::invoke(std::forward<F>(f), obj, get<Is>(args)...);
}

} // namespace detail

template <typename TFunction, typename TVector>
inline auto call_function_with_message_args(const TFunction& f, const TVector& t)
{
    constexpr std::size_t arity = message_args_size<TVector>::value;
    return detail::call_with_msg_args_impl(
        f,
        t,
        std::make_index_sequence<arity>{});
}

template <typename TObj, typename TFunction, typename TVector>
inline auto call_function_with_message_args(TObj& obj, const TFunction& f, const TVector& t)
{
    constexpr std::size_t arity = message_args_size<TVector>::value;
    return detail::call_with_msg_args_impl(
        obj,
        f,
        t,
        std::make_index_sequence<arity>{});
}


} // namespace sintra;



