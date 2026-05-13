// Force-included into libldac translation units on MSVC.
// Maps a few GCC-only attributes used by AOSP source to no-ops.
#pragma once
#ifdef _MSC_VER
  #ifndef __attribute__
    #define __attribute__(x)
  #endif
#endif
