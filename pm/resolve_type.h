/*
Copyright 2017 Ioannis Makris

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef __SINTRA_RESOLVE_TYPE_H__
#define __SINTRA_RESOLVE_TYPE_H__


namespace sintra {


template<typename RT, typename OBJECT_TYPE, typename... Args>
constexpr static RT resolve_rt(RT(OBJECT_TYPE::*v)(Args...)) {  }


template<typename RT, typename OBJECT_TYPE, typename... Args>
constexpr static OBJECT_TYPE resolve_object_type(RT(OBJECT_TYPE::*v)(Args...)) {  }


template<typename RT, typename OBJECT_TYPE, typename... Args>
constexpr static RT resolve_rt(RT(OBJECT_TYPE::*v)(Args...) const) {  }


template<typename RT, typename OBJECT_TYPE, typename... Args>
constexpr static OBJECT_TYPE resolve_object_type(RT(OBJECT_TYPE::*v)(Args...) const) {  }


template<template<typename...> typename TYPE_CONTAINER,
         typename RT, typename OBJECT_TYPE, typename... Args>
constexpr static TYPE_CONTAINER<Args...> resolve_args(RT(OBJECT_TYPE::*v)(Args...)) {  }


template<template<typename...> typename TYPE_CONTAINER,
         typename RT, typename OBJECT_TYPE, typename... Args>
constexpr static TYPE_CONTAINER<Args...> resolve_args(RT(OBJECT_TYPE::*v)(Args...) const) {  }


template<template<typename...> typename TYPE_CONTAINER,
         typename RT, typename... Args>
constexpr static TYPE_CONTAINER<Args...> resolve_args(RT(*v)(Args...)) {  }


template<typename RT, typename OBJECT_TYPE, typename ARG_TYPE>
constexpr static ARG_TYPE resolve_single_arg(RT(OBJECT_TYPE::*v)(const ARG_TYPE&) const) {  }


template<typename RT, typename OBJECT_TYPE, typename ARG_TYPE>
constexpr static ARG_TYPE resolve_single_arg(RT(OBJECT_TYPE::*v)(ARG_TYPE) const) {  }


template<typename RT, typename ARG_TYPE>
constexpr static ARG_TYPE resolve_single_arg(RT(*v)(ARG_TYPE)) {  }


template <typename LAMBDA_TYPE>
constexpr static decltype(resolve_single_arg(&LAMBDA_TYPE::operator()))
resolve_single_functor_arg(const LAMBDA_TYPE& lt) {}


template<typename VAR_TYPE, typename OBJECT_TYPE>
constexpr static VAR_TYPE resolve_var_type(VAR_TYPE OBJECT_TYPE::*v) {}


template<typename VAR_TYPE, typename OBJECT_TYPE>
constexpr static OBJECT_TYPE resolve_object_type(VAR_TYPE OBJECT_TYPE::*v) {}

} // namespace sintra

#endif
