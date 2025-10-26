#pragma once

// Ensure that RTTI is available. Different compilers expose different feature
// macros for this, so we check the variants that they define.
#if defined(__clang__)
#  if !__has_feature(cxx_rtti)
#    error "sintra requires runtime type information (RTTI). Enable it by removing -fno-rtti or passing -frtti."
#  endif
#elif defined(__GNUC__)
#  if !defined(__GXX_RTTI)
#    error "sintra requires runtime type information (RTTI). Enable it by removing -fno-rtti or passing -frtti."
#  endif
#elif defined(_MSC_VER)
#  if !defined(_CPPRTTI)
#    error "sintra requires runtime type information (RTTI). Enable it by compiling with /GR."
#  endif
#else
#  if defined(__cpp_rtti)
     // RTTI support is advertised via the standard feature macro.
#  else
#    error "sintra requires runtime type information (RTTI). Please enable RTTI for your toolchain."
#  endif
#endif

namespace sintra::detail {
inline constexpr bool rtti_is_enabled = true;
}
