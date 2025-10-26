#pragma once

#include <string>
#include <typeinfo>
#include <utility>

#if defined(__clang__)
#  if !__has_feature(cxx_rtti)
#    error "Sintra requires RTTI. Enable it with -frtti (it is on by default)."
#  endif
#elif defined(_MSC_VER)
#  ifndef _CPPRTTI
#    error "Sintra requires RTTI. Enable it with /GR."
#  endif
#elif defined(__INTEL_COMPILER) || defined(__ICC) || defined(__INTEL_LLVM_COMPILER)
#  if !defined(__RTTI) && !defined(__INTEL_RTTI__)
#    error "Sintra requires RTTI. Enable it with -frtti (it is on by default)."
#  endif
#elif defined(__GNUC__)
#  ifndef __GXX_RTTI
#    error "Sintra requires RTTI. Enable it with -frtti (it is on by default)."
#  endif
#else
#  if !defined(__cpp_rtti)
#    error "Sintra requires RTTI. Ensure that runtime type information is enabled."
#  endif
#endif

#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>

#if defined(__has_include)
#  if __has_include(<cxxabi.h>)
#    include <cxxabi.h>
#    define SINTRA_HAS_CXA_DEMANGLE 1
#  endif
#endif

namespace sintra {
namespace detail {

inline std::string demangle_type_name(const char* name)
{
#if defined(SINTRA_HAS_CXA_DEMANGLE)
    int status = 0;
    std::unique_ptr<char, void(*)(void*)> demangled(
        abi::__cxa_demangle(name, nullptr, nullptr, &status),
        std::free);
    if (status == 0 && demangled) {
        return std::string(demangled.get());
    }
#endif
    return name ? std::string(name) : std::string();
}

inline std::string type_pretty_name(const std::type_info& ti)
{
    return demangle_type_name(ti.name());
}

template <typename T>
inline std::string type_pretty_name()
{
    return type_pretty_name(typeid(T));
}

inline std::string compiler_tag()
{
    std::ostringstream oss;
#if defined(__MINGW32__)
#  if defined(__clang__)
    oss << "clang-mingw-" << __clang_version__;
#  elif defined(__GNUC__)
    oss << "gcc-mingw-" << __VERSION__;
#  else
    oss << "mingw-unknown";
#  endif
#elif defined(__INTEL_LLVM_COMPILER)
    oss << "intel-llvm-" << __INTEL_LLVM_COMPILER;
#elif defined(__INTEL_COMPILER)
    oss << "intel-classic-" << __INTEL_COMPILER;
#elif defined(__clang__)
    oss << "clang-" << __clang_version__;
#elif defined(_MSC_VER)
    oss << "msvc-" << _MSC_VER;
#elif defined(__GNUC__)
    oss << "gcc-" << __VERSION__;
#else
    oss << "unknown-compiler";
#endif
    return oss.str();
}

inline void append_stdlib_tag(std::ostringstream& oss)
{
#if defined(__GLIBCXX__)
    oss << "|libstdc++-" << __GLIBCXX__;
#  if defined(_GLIBCXX_USE_CXX11_ABI)
    oss << ( _GLIBCXX_USE_CXX11_ABI ? "+new" : "+old" );
#  else
    oss << "+legacy";
#  endif
#elif defined(_LIBCPP_VERSION)
    oss << "|libc++-" << _LIBCPP_VERSION;
#elif defined(_MSVC_STL_VERSION)
    oss << "|msvc-stl-" << _MSVC_STL_VERSION;
#else
    oss << "|unknown-stl";
#endif
}

inline void append_platform_tag(std::ostringstream& oss)
{
#if defined(_WIN32)
    oss << "|win";
#elif defined(__APPLE__)
    oss << "|apple";
#elif defined(__linux__)
    oss << "|linux";
#else
    oss << "|unknown-os";
#endif
}

inline void append_pointer_size(std::ostringstream& oss)
{
#if defined(__SIZEOF_POINTER__)
    oss << "|ptr" << (static_cast<int>(__SIZEOF_POINTER__) * 8);
#elif defined(_WIN64)
    oss << "|ptr64";
#elif defined(_WIN32)
    oss << "|ptr32";
#endif
}

inline void append_abi_version(std::ostringstream& oss)
{
#if defined(__GXX_ABI_VERSION)
    oss << "|gxxabi-" << __GXX_ABI_VERSION;
#endif
}

inline std::string make_abi_tag()
{
    std::ostringstream oss;
    oss << compiler_tag();
    append_stdlib_tag(oss);
    append_platform_tag(oss);
    append_pointer_size(oss);
    append_abi_version(oss);
    return oss.str();
}

inline const std::string& local_abi_tag()
{
    static const std::string tag = make_abi_tag();
    return tag;
}

} // namespace detail
} // namespace sintra

#undef SINTRA_HAS_CXA_DEMANGLE

