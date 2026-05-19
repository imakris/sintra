// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

namespace sintra {


// Member-function / free-function pointer introspection.
//
// Lets every const/non-const member-function-pointer overload pair in the
// transceiver layer collapse to a single template that deduces F and pulls
// the return type, class type, and argument pack from the trait.
template <typename F>
struct member_fn_traits;

template <typename RT, typename OBJECT_T, typename... Args>
struct member_fn_traits<RT(OBJECT_T::*)(Args...)>
{
    using return_type = RT;
    using class_type  = OBJECT_T;

    template <template<typename...> typename TYPE_CONTAINER>
    using args_as = TYPE_CONTAINER<Args...>;
};

template <typename RT, typename OBJECT_T, typename... Args>
struct member_fn_traits<RT(OBJECT_T::*)(Args...) const>
    : member_fn_traits<RT(OBJECT_T::*)(Args...)>
{};

template <typename RT, typename... Args>
struct member_fn_traits<RT(*)(Args...)>
{
    using return_type = RT;
    using class_type  = void;

    template <template<typename...> typename TYPE_CONTAINER>
    using args_as = TYPE_CONTAINER<Args...>;
};


template <typename F>
constexpr static auto resolve_rt(F) -> typename member_fn_traits<F>::return_type {}


template <typename F>
constexpr static auto resolve_object_type(F) -> typename member_fn_traits<F>::class_type {}


template <template<typename...> typename TYPE_CONTAINER, typename F>
constexpr static auto resolve_args(F)
    -> typename member_fn_traits<F>::template args_as<TYPE_CONTAINER> {}


template<typename RT, typename OBJECT_T, typename ARG_T>
constexpr static ARG_T resolve_single_arg(RT(OBJECT_T::*)(const ARG_T&) const) {  }


template<typename RT, typename OBJECT_T, typename ARG_T>
constexpr static ARG_T resolve_single_arg(RT(OBJECT_T::*)(ARG_T) const) {  }


template<typename RT, typename ARG_T>
constexpr static ARG_T resolve_single_arg(RT(*)(ARG_T)) {  }


template <typename LAMBDA_T>
constexpr static decltype(resolve_single_arg(&LAMBDA_T::operator()))
resolve_single_functor_arg(const LAMBDA_T&) {}


} // namespace sintra

