#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <typeinfo>

#if defined(__GNUG__)
#include <cxxabi.h>
#endif

namespace sintra {
namespace detail {

#if !defined(__cpp_rtti) && !defined(__GXX_RTTI) && !defined(_CPPRTTI) && !defined(__RTTI)
#error "Sintra requires RTTI. Please enable Run-Time Type Information (for example remove -fno-rtti or compile with /GR)."
#endif

inline std::string demangle_type_name(const char* name)
{
#if defined(__GNUG__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    if (status == 0 && demangled) {
        std::string result(demangled);
        std::free(demangled);
        return result;
    }
    if (demangled) {
        std::free(demangled);
    }
    return name;
#else
    return name;
#endif
}

inline std::string pretty_type_name(const std::type_info& ti)
{
    return demangle_type_name(ti.name());
}

template <typename T>
std::string pretty_type_name()
{
    return pretty_type_name(typeid(T));
}

inline std::string compiler_tag()
{
#if defined(__clang__)
    return "Clang " + std::to_string(__clang_major__) + "." +
        std::to_string(__clang_minor__) + "." + std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "." +
        std::to_string(__GNUC_MINOR__) + "." + std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "Unknown compiler";
#endif
}

inline std::string standard_library_tag()
{
#if defined(_LIBCPP_VERSION)
    return "libc++ " + std::to_string(_LIBCPP_VERSION);
#elif defined(__GLIBCXX__)
    return "libstdc++ " + std::to_string(__GLIBCXX__);
#elif defined(_CPPLIB_VER)
#  if defined(_MSVC_STL_VERSION)
    return "MSVC STL " + std::to_string(_MSVC_STL_VERSION);
#  else
    return "MSVC STL " + std::to_string(_CPPLIB_VER);
#  endif
#else
    return "Unknown standard library";
#endif
}

inline std::string platform_tag()
{
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "Apple";
#elif defined(__linux__)
    return "Linux";
#elif defined(__unix__)
    return "Unix";
#else
    return "Unknown platform";
#endif
}

inline std::string libc_tag()
{
#if defined(__GLIBC__)
    return "glibc " + std::to_string(__GLIBC__) + "." + std::to_string(__GLIBC_MINOR__);
#elif defined(_WIN32)
    return "MSVCRT";
#elif defined(__APPLE__)
    return "libSystem";
#else
    return "Unknown libc";
#endif
}

inline std::string endian_tag()
{
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
    if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) {
        return "little";
    }
    if (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) {
        return "big";
    }
    return "mixed";
#elif defined(_WIN32)
    return "little";
#else
    return "unknown";
#endif
}

inline std::string current_abi_tag()
{
    std::string tag;
    tag.reserve(96);
    tag.append("compiler=").append(compiler_tag());
    tag.append(";stdlib=").append(standard_library_tag());
    tag.append(";libc=").append(libc_tag());
    tag.append(";platform=").append(platform_tag());
    tag.append(";arch=").append(std::to_string(sizeof(void*) * 8)).append("-bit");
    tag.append(";endian=").append(endian_tag());
    return tag;
}

inline std::string make_type_key(const std::string& abi_tag, const std::string& type_name)
{
    std::string key;
    key.reserve(abi_tag.size() + 1 + type_name.size());
    key.append(abi_tag);
    key.push_back('\n');
    key.append(type_name);
    return key;
}

template <typename T>
std::string type_key()
{
    return make_type_key(current_abi_tag(), pretty_type_name<T>());
}

inline bool split_type_key(const std::string& key, std::string& abi_tag_out, std::string& type_name_out)
{
    const auto pos = key.find('\n');
    if (pos == std::string::npos) {
        return false;
    }
    abi_tag_out = key.substr(0, pos);
    type_name_out = key.substr(pos + 1);
    return true;
}

} // namespace detail
} // namespace sintra
