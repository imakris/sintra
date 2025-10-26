// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>

#if defined(__GNUG__)
#include <cxxabi.h>
#endif

namespace sintra::detail {

namespace detail_type_utils {

template <typename, typename = void>
struct is_complete : std::false_type {};

template <typename T>
struct is_complete<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};

template <>
struct is_complete<void, void> : std::true_type {};

template <>
struct is_complete<const void, void> : std::true_type {};

template <>
struct is_complete<volatile void, void> : std::true_type {};

template <>
struct is_complete<const volatile void, void> : std::true_type {};

template <typename T>
constexpr std::string_view ctti_name()
{
#if defined(__clang__) || defined(__GNUC__)
    constexpr std::string_view pretty = __PRETTY_FUNCTION__;
    constexpr std::string_view needle = "T = ";
    auto start = pretty.find(needle);
    if (start == std::string_view::npos) {
        return pretty;
    }
    start += needle.size();
    auto end = pretty.rfind(']');
    if (end == std::string_view::npos || end < start) {
        end = pretty.size();
    }
    return pretty.substr(start, end - start);
#elif defined(_MSC_VER)
    constexpr std::string_view pretty = __FUNCSIG__;
    constexpr std::string_view needle = "ctti_name<";
    auto begin = pretty.find(needle);
    if (begin == std::string_view::npos) {
        return pretty;
    }
    begin += needle.size();
    auto end = begin;
    int depth = 0;
    for (; end < pretty.size(); ++end) {
        const auto ch = pretty[end];
        if (ch == '<') {
            ++depth;
        } else if (ch == '>') {
            if (depth == 0) {
                break;
            }
            --depth;
        }
    }
    if (end >= pretty.size()) {
        end = pretty.size();
    }
    while (begin < end && pretty[begin] == ' ') {
        ++begin;
    }
    while (end > begin && pretty[end - 1] == ' ') {
        --end;
    }
    return pretty.substr(begin, end - begin);
#else
    return std::string_view{};
#endif
}

inline std::string demangle(const char* name)
{
#if defined(__GNUG__)
    int status = 0;
    std::unique_ptr<char, void(*)(void*)> demangled(
        abi::__cxa_demangle(name, nullptr, nullptr, &status),
        &std::free);
    if (status == 0 && demangled) {
        return demangled.get();
    }
#else
    (void)name;
#endif
    return name ? name : std::string{};
}

inline std::string compiler_identity()
{
#if defined(_MSC_VER)
    std::ostringstream oss;
    oss << "msvc-" << _MSC_VER;
    return oss.str();
#elif defined(__clang__)
    std::ostringstream oss;
    oss << "clang-" << __clang_major__ << '.' << __clang_minor__ << '.' << __clang_patchlevel__;
    return oss.str();
#elif defined(__GNUC__)
    std::ostringstream oss;
    oss << "gnu-" << __GNUC__ << '.' << __GNUC_MINOR__ << '.' << __GNUC_PATCHLEVEL__;
    return oss.str();
#elif defined(__INTEL_COMPILER)
    std::ostringstream oss;
    oss << "intel-" << __INTEL_COMPILER;
    return oss.str();
#else
    return "unknown-compiler";
#endif
}

inline std::string stdlib_identity()
{
#if defined(_MSVC_STL_VERSION)
    std::ostringstream oss;
    oss << "msvc-stl-" << _MSVC_STL_VERSION;
    return oss.str();
#elif defined(_LIBCPP_VERSION)
    std::ostringstream oss;
    oss << "libc++-" << _LIBCPP_VERSION;
    return oss.str();
#elif defined(__GLIBCXX__)
    std::ostringstream oss;
    oss << "libstdc++-" << __GLIBCXX__;
    return oss.str();
#else
    return "unknown-stdlib";
#endif
}

inline std::string platform_identity()
{
#if defined(__MINGW64__)
    return "windows-mingw64";
#elif defined(__MINGW32__)
    return "windows-mingw";
#elif defined(_WIN32)
    return "windows-msvc";
#elif defined(__APPLE__)
    return "apple";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__NetBSD__)
    return "netbsd";
#elif defined(__OpenBSD__)
    return "openbsd";
#elif defined(__EMSCRIPTEN__)
    return "emscripten";
#else
    return "unknown-platform";
#endif
}

inline std::string architecture_identity()
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__ppc64__) || defined(__powerpc64__)
    return "ppc64";
#elif defined(__ppc__) || defined(__powerpc__)
    return "ppc";
#elif defined(__riscv)
    return "riscv";
#else
    return "unknown-arch";
#endif
}

inline std::string describe_token_component(const std::string& component)
{
    auto equals = component.find('=');
    if (equals == std::string::npos) {
        return component;
    }

    std::string name = component.substr(0, equals);
    std::string value = component.substr(equals + 1);

    if (name == "compiler") {
        return "compiler " + value;
    }
    if (name == "stdlib") {
        return "standard library " + value;
    }
    if (name == "platform") {
        return "platform " + value;
    }
    if (name == "arch") {
        return "architecture " + value;
    }

    return name + ' ' + value;
}

} // namespace detail_type_utils

inline std::string type_name_from_info(const std::type_info& info)
{
    return detail_type_utils::demangle(info.name());
}

template <typename T>
inline std::string type_name()
{
    if constexpr (detail_type_utils::is_complete<T>::value) {
        return type_name_from_info(typeid(T));
    }
    else {
        constexpr auto view = detail_type_utils::ctti_name<T>();
        return std::string(view);
    }
}

inline std::string abi_token()
{
    std::ostringstream oss;
    oss << "compiler=" << detail_type_utils::compiler_identity();
    oss << ";stdlib=" << detail_type_utils::stdlib_identity();
    oss << ";platform=" << detail_type_utils::platform_identity();
    oss << ";arch=" << detail_type_utils::architecture_identity();
    return oss.str();
}

inline std::string abi_description()
{
    std::ostringstream oss;
    oss << "compiler " << detail_type_utils::compiler_identity();
    oss << ", standard library " << detail_type_utils::stdlib_identity();
    oss << ", platform " << detail_type_utils::platform_identity();
    oss << ", architecture " << detail_type_utils::architecture_identity();
    return oss.str();
}

inline std::string describe_abi_token(const std::string& token)
{
    if (token.empty()) {
        return std::string("(unspecified ABI)");
    }

    std::ostringstream oss;
    bool first = true;
    std::size_t start = 0;
    while (start < token.size()) {
        auto next = token.find(';', start);
        auto component = token.substr(start, next == std::string::npos ? std::string::npos : next - start);
        auto readable = detail_type_utils::describe_token_component(component);
        if (!readable.empty()) {
            if (!first) {
                oss << ", ";
            }
            oss << readable;
            first = false;
        }
        if (next == std::string::npos) {
            break;
        }
        start = next + 1;
    }

    auto description = oss.str();
    if (description.empty()) {
        return token;
    }
    return description;
}

inline const char* abi_marker_filename()
{
    return "sintra_abi.info";
}

} // namespace sintra::detail

