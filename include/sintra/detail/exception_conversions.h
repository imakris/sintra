// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <algorithm>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>

#include "globals.h"


namespace sintra {

template<typename = void, typename... Args>
struct is_string_convertible : std::false_type {};

template<typename... Args>
struct is_string_convertible<std::void_t<decltype(std::to_string(std::declval<Args>()...))>, Args...>
    : std::true_type {};

template<typename... Args>
inline constexpr bool is_string_convertible_v = is_string_convertible<void, Args...>::value;

template <
    typename T,
    typename = std::enable_if_t<std::is_base_of<std::exception, T>::value>
>
std::pair<type_id_type, std::string> exception_to_string(const T& ex);

template <
    typename T,
    typename = std::enable_if_t<!std::is_base_of<std::exception, T>::value>,
    typename = std::enable_if_t<is_string_convertible_v<T> >
>
std::pair<type_id_type, std::string> exception_to_string(const T& ex);

template <
    typename T,
    typename = std::enable_if_t<!std::is_base_of<std::exception, T>::value>,
    typename = std::enable_if_t<!is_string_convertible_v<T> >,
    typename = void
>
std::pair<type_id_type, std::string> exception_to_string(const T& ex);

inline
void string_to_exception(type_id_type exception_type, const std::string& str);

}


