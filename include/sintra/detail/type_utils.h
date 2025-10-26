#pragma once

#include "rtti_check.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <typeinfo>

#if defined(__GNUG__)
#    include <cxxabi.h>
#endif

#include <memory>
#include <sstream>

namespace sintra::detail {

inline std::string demangle(const char* name)
{
#if defined(__GNUG__)
    int status = 0;
    std::unique_ptr<char, decltype(&std::free)> buffer{abi::__cxa_demangle(name, nullptr, nullptr, &status), &std::free};
    if (status == 0 && buffer) {
        return std::string(buffer.get());
    }
#endif
    return std::string(name);
}

inline std::string type_pretty_name(const std::type_info& info)
{
    return demangle(info.name());
}

template <typename T>
std::string type_pretty_name()
{
    return type_pretty_name(typeid(T));
}

inline std::string compiler_identifier()
{
#if defined(__clang__) && defined(_MSC_VER)
    std::string id = "clang-cl-";
    id += __clang_version__;
    id += "-msvc";
    return id;
#elif defined(__clang__)
    std::string id = "clang-";
    id += __clang_version__;
#  if defined(__MINGW32__)
    id += "-mingw";
#  elif defined(__apple_build_version__)
    id += "-apple";
#  endif
    return id;
#elif defined(_MSC_VER)
    std::string id = "msvc-";
    id += std::to_string(_MSC_FULL_VER);
#  if defined(_MSC_BUILD)
    id += ".";
    id += std::to_string(_MSC_BUILD);
#  endif
    return id;
#elif defined(__MINGW64__)
    std::string id = "mingw64-gcc-";
    id += std::to_string(__GNUC__);
    id += ".";
    id += std::to_string(__GNUC_MINOR__);
    id += ".";
    id += std::to_string(__GNUC_PATCHLEVEL__);
    return id;
#elif defined(__MINGW32__)
    std::string id = "mingw-gcc-";
    id += std::to_string(__GNUC__);
    id += ".";
    id += std::to_string(__GNUC_MINOR__);
    id += ".";
    id += std::to_string(__GNUC_PATCHLEVEL__);
    return id;
#elif defined(__GNUC__)
    std::string id = "gcc-";
    id += std::to_string(__GNUC__);
    id += ".";
    id += std::to_string(__GNUC_MINOR__);
    id += ".";
    id += std::to_string(__GNUC_PATCHLEVEL__);
    return id;
#else
    return "unknown-compiler";
#endif
}

inline std::string standard_library_identifier()
{
#if defined(_CPPLIB_VER)
    std::string id = "msvcstd-";
    id += std::to_string(_CPPLIB_VER);
    return id;
#elif defined(_LIBCPP_VERSION)
    std::string id = "libc++-";
    id += std::to_string(_LIBCPP_VERSION);
    return id;
#elif defined(__GLIBCXX__)
    std::string id = "libstdc++-";
    id += std::to_string(__GLIBCXX__);
    return id;
#else
    return "unknown-stdlib";
#endif
}

inline std::string platform_identifier()
{
#if defined(_WIN32)
#  if defined(__MINGW32__)
    return "windows-mingw";
#  elif defined(__clang__)
    return "windows-clang";
#  else
    return "windows";
#  endif
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#elif defined(__unix__)
    return "unix";
#else
    return "unknown-platform";
#endif
}

inline std::string architecture_identifier()
{
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64";
#elif defined(_M_ARM) || defined(__arm__)
    return "arm";
#elif defined(__powerpc64__)
    return "ppc64";
#elif defined(__powerpc__)
    return "ppc";
#else
    return "unknown-arch";
#endif
}

inline bool is_little_endian()
{
    const std::uint16_t value = 0x0102;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    return bytes[0] == 0x02;
}

inline const std::string& current_abi_descriptor()
{
    static const std::string descriptor = [] {
        std::ostringstream oss;
        oss << "compiler=" << compiler_identifier()
            << ";stdlib=" << standard_library_identifier()
            << ";platform=" << platform_identifier()
            << ";arch=" << architecture_identifier()
            << ";pointer_size=" << sizeof(void*)
            << ";endianness=" << (is_little_endian() ? "little" : "big");
        return oss.str();
    }();
    return descriptor;
}

} // namespace sintra::detail
