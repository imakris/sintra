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
    #define _WIN32_WINNT 0x0600
  #elif _WIN32_WINNT < 0x0600
    #undef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
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

  extern "C" {
  WINBASEAPI BOOL WINAPI InitializeProcThreadAttributeList(
      LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList,
      DWORD dwAttributeCount,
      DWORD dwFlags,
      PSIZE_T lpSize);

  WINBASEAPI BOOL WINAPI UpdateProcThreadAttributeList(
      LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList,
      DWORD dwFlags,
      DWORD_PTR Attribute,
      PVOID lpValue,
      SIZE_T cbSize,
      PVOID lpPreviousValue,
      PSIZE_T lpReturnSize);

  WINBASEAPI VOID WINAPI DeleteProcThreadAttributeList(
      LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList);
  }
#endif
