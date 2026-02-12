// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

namespace sintra {


template<typename RT, typename OBJECT_T, typename... Args>
constexpr static RT resolve_rt(RT(OBJECT_T::*)(Args...)) {  }


template<typename RT, typename OBJECT_T, typename... Args>
constexpr static OBJECT_T resolve_object_type(RT(OBJECT_T::*)(Args...)) {  }


template<typename RT, typename OBJECT_T, typename... Args>
constexpr static RT resolve_rt(RT(OBJECT_T::*)(Args...) const) {  }


template<typename RT, typename OBJECT_T, typename... Args>
constexpr static OBJECT_T resolve_object_type(RT(OBJECT_T::*)(Args...) const) {  }


template<
    template<typename...> typename TYPE_CONTAINER,
    typename RT,
    typename OBJECT_T,
    typename... Args
>
constexpr static TYPE_CONTAINER<Args...> resolve_args(RT(OBJECT_T::*)(Args...)) {  }


template<
    template<typename...> typename TYPE_CONTAINER,
    typename RT,
    typename OBJECT_T,
    typename... Args
>
constexpr static TYPE_CONTAINER<Args...> resolve_args(RT(OBJECT_T::*)(Args...) const) {  }


template<
    template<typename...> typename TYPE_CONTAINER,
    typename RT,
    typename... Args
>
constexpr static TYPE_CONTAINER<Args...> resolve_args(RT(*)(Args...)) {  }


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

