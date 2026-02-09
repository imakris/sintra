// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "exception_conversions.h"
#include "messaging/message.h"
#include "type_utils.h"

#include <filesystem>
#include <future>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <typeinfo>
#include <unordered_map>
#include <utility>


namespace sintra {

namespace fs = std::filesystem;

using std::string;
using std::unordered_map;

template <
    typename T,
    typename /*= std::enable_if_t<is_base_of<std::exception, T>::value>*/
>
std::pair<type_id_type, std::string> exception_to_string(const T& ex)
{
    return std::make_pair(get_type_id<T>(), ex.what());
}


template <
    typename T,
    typename /*= std::enable_if_t<!is_base_of<std::exception, T>::value>*/,
    typename /*= std::enable_if_t<std::is_same<std::invoke_result<decltype(std::to_string),T>::type, std::string>::value>*/
>
std::pair<type_id_type, std::string> exception_to_string(const T& ex)
{
    return std::make_pair(get_type_id<message_string>(), std::to_string(ex));
}



template <
    typename T,
    typename /*= std::enable_if_t<!is_base_of<std::exception, T>::value>*/,
    typename /*= std::enable_if_t<!std::is_same<std::invoke_result<decltype(std::to_string),T>::type, std::string>::value>*/,
    typename /*= void*/
>
std::pair<type_id_type, std::string> exception_to_string(const T& ex)
{
    return std::make_pair(
        get_type_id<message_string>(),
        std::string("Exception of type ") +
        detail::type_name<T>() +
        ", which is not serialized by sintra"
    );
}



//=======================



inline
void throw_generic_exception(const std::string& what)
{
    throw std::runtime_error(what);
}

template <typename T>
void throw_typed_exception(const std::string& what)
{
    throw T(what);
}

inline
void string_to_exception(type_id_type exception_type, const std::string& str)
{
#define SINTRA_EXCEPTION_COMMON_LIST(X) \
    X(std_invalid_argument, std::invalid_argument) \
    X(std_domain_error, std::domain_error) \
    X(std_length_error, std::length_error) \
    X(std_out_of_range, std::out_of_range) \
    X(std_range_error, std::range_error) \
    X(std_overflow_error, std::overflow_error) \
    X(std_underflow_error, std::underflow_error) \
    X(std_ios_base_failure, std::ios_base::failure) \
    X(std_logic_error, std::logic_error) \
    X(std_runtime_error, std::runtime_error)

    if (exception_type < static_cast<type_id_type>(detail::reserved_id::num_reserved_type_ids)) {
        switch (static_cast<detail::reserved_id>(exception_type)) {
#define SINTRA_EXCEPTION_SWITCH_CASE(reserved_id_value, exception_type_name) \
        case detail::reserved_id::reserved_id_value: \
            throw_typed_exception<exception_type_name>(str);
        SINTRA_EXCEPTION_COMMON_LIST(SINTRA_EXCEPTION_SWITCH_CASE)
#undef SINTRA_EXCEPTION_SWITCH_CASE
        case detail::reserved_id::std_exception:
        case detail::reserved_id::unknown_exception:
            throw_generic_exception(str);
        default:
            break;
        }
    }

    static unordered_map<type_id_type, void(*)(const std::string&)> ex_map = {
#define SINTRA_EXCEPTION_MAP_ENTRY(reserved_id_value, exception_type_name) \
        {get_type_id<exception_type_name>(), throw_typed_exception<exception_type_name>},
        SINTRA_EXCEPTION_COMMON_LIST(SINTRA_EXCEPTION_MAP_ENTRY)
#undef SINTRA_EXCEPTION_MAP_ENTRY
        {get_type_id<std::system_error        >(), throw_typed_exception<std::runtime_error    >},

        // For the following exceptions, due to non-trivial constructor, their parent type is thrown
        {get_type_id<std::future_error        >(), throw_typed_exception<std::logic_error      >},
        {get_type_id<std::regex_error         >(), throw_typed_exception<std::runtime_error    >},
        {get_type_id<fs::filesystem_error     >(), throw_typed_exception<std::runtime_error    >},

        // For the following exceptions, due to non-trivial constructor, a generic std::exception is thrown
        {get_type_id<std::bad_optional_access >(), throw_generic_exception},
        {get_type_id<std::bad_typeid          >(), throw_generic_exception},
        {get_type_id<std::bad_cast            >(), throw_generic_exception},
        {get_type_id<std::bad_weak_ptr        >(), throw_generic_exception},
        {get_type_id<std::bad_function_call   >(), throw_generic_exception},
        {get_type_id<std::bad_alloc           >(), throw_generic_exception},
        {get_type_id<std::bad_array_new_length>(), throw_generic_exception},
        {get_type_id<std::bad_exception       >(), throw_generic_exception},
        {get_type_id<std::bad_variant_access  >(), throw_generic_exception}
    };

#undef SINTRA_EXCEPTION_COMMON_LIST

    auto it = ex_map.find(exception_type);
    if (it != ex_map.end()) {
        it->second(str);
    }
    else {
        throw_generic_exception(str);
    }
}

} // namespae sintra

