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
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif
