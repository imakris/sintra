#pragma once
/*
sintra_windows.h

Centralized Windows.h inclusion with consistent preprocessor hygiene.

This header ensures all Windows API usage in sintra gets the same treatment:
  - NOMINMAX:            Prevents min/max macro pollution from Windows.h
  - WIN32_LEAN_AND_MEAN: Excludes rarely-used Windows APIs for faster compilation

Usage:
  Replace direct `#include <windows.h>` with `#include "sintra_windows.h"`
  in all sintra headers and source files.
*/

#if defined(_WIN32)
  #if !defined(_WIN32_WINNT)
    #define _WIN32_WINNT 0x0601
  #elif _WIN32_WINNT < 0x0601
    #undef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601
  #endif
  #if !defined(WINVER) || (WINVER < _WIN32_WINNT)
    #undef WINVER
    #define WINVER _WIN32_WINNT
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <processthreadsapi.h>

#endif
