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
/*


// For the following exceptions, due to non-trivial constructor, a generic std::exception is thrown



{get_type_id<std::invalid_argument    >(), throw_typed_exception<std::invalid_argument >},
{get_type_id<std::domain_error        >(), throw_typed_exception<std::domain_error     >},
{get_type_id<std::length_error        >(), throw_typed_exception<std::length_error     >},
{get_type_id<std::out_of_range        >(), throw_typed_exception<std::out_of_range     >},
{get_type_id<std::range_error         >(), throw_typed_exception<std::range_error      >},
{get_type_id<std::overflow_error      >(), throw_typed_exception<std::overflow_error   >},
{get_type_id<std::underflow_error     >(), throw_typed_exception<std::underflow_error  >},
{get_type_id<std::ios_base::failure   >(), throw_typed_exception<std::ios_base::failure>},


{get_type_id<std::logic_error         >(), throw_typed_exception<std::logic_error      >},
{get_type_id<std::runtime_error       >(), throw_typed_exception<std::runtime_error    >},

// Due to their non-trivial constructor, the following exceptions are handled with their parent type
std::future_error         -> std::logic_error  
std::regex_error          -> std::runtime_error
std::system_error         -> std::runtime_error
fs::filesystem_error      -> std::runtime_error
std::bad_optional_access  -> std::exception
std::bad_typeid           -> std::exception
std::bad_cast             -> std::exception
std::bad_weak_ptr         -> std::exception
std::bad_function_call    -> std::exception
std::bad_alloc            -> std::exception
std::bad_array_new_length -> std::exception
std::bad_exception        -> std::exception
std::bad_variant_access   -> std::exception


*/

inline
void string_to_exception(type_id_type exception_type, const std::string& str)
{
    if (exception_type < static_cast<type_id_type>(detail::reserved_id::num_reserved_type_ids)) {
        switch (static_cast<detail::reserved_id>(exception_type)) {
        case detail::reserved_id::std_invalid_argument:
            throw_typed_exception<std::invalid_argument>(str);
        case detail::reserved_id::std_domain_error:
            throw_typed_exception<std::domain_error>(str);
        case detail::reserved_id::std_length_error:
            throw_typed_exception<std::length_error>(str);
        case detail::reserved_id::std_out_of_range:
            throw_typed_exception<std::out_of_range>(str);
        case detail::reserved_id::std_range_error:
            throw_typed_exception<std::range_error>(str);
        case detail::reserved_id::std_overflow_error:
            throw_typed_exception<std::overflow_error>(str);
        case detail::reserved_id::std_underflow_error:
            throw_typed_exception<std::underflow_error>(str);
        case detail::reserved_id::std_ios_base_failure:
            throw_typed_exception<std::ios_base::failure>(str);
        case detail::reserved_id::std_logic_error:
            throw_typed_exception<std::logic_error>(str);
        case detail::reserved_id::std_runtime_error:
            throw_typed_exception<std::runtime_error>(str);
        case detail::reserved_id::std_exception:
        case detail::reserved_id::unknown_exception:
            throw_generic_exception(str);
        default:
            break;
        }
    }

    static unordered_map<type_id_type, void(*)(const std::string&)> ex_map = {
        {get_type_id<std::logic_error         >(), throw_typed_exception<std::logic_error      >},
        {get_type_id<std::invalid_argument    >(), throw_typed_exception<std::invalid_argument >},
        {get_type_id<std::domain_error        >(), throw_typed_exception<std::domain_error     >},
        {get_type_id<std::length_error        >(), throw_typed_exception<std::length_error     >},
        {get_type_id<std::out_of_range        >(), throw_typed_exception<std::out_of_range     >},
        {get_type_id<std::runtime_error       >(), throw_typed_exception<std::runtime_error    >},
        {get_type_id<std::range_error         >(), throw_typed_exception<std::range_error      >},
        {get_type_id<std::overflow_error      >(), throw_typed_exception<std::overflow_error   >},
        {get_type_id<std::underflow_error     >(), throw_typed_exception<std::underflow_error  >},
        {get_type_id<std::system_error        >(), throw_typed_exception<std::runtime_error    >},
        {get_type_id<std::ios_base::failure   >(), throw_typed_exception<std::ios_base::failure>},

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

    auto it = ex_map.find(exception_type);
    if (it != ex_map.end()) {
        it->second(str);
    }
    else {
        throw_generic_exception(str);
    }
}

} // namespae sintra

