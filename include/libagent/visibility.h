#pragma once

// Symbol export/visibility control for libagent.
//
// When building libagent as a shared library, CMake automatically defines
// libagent_EXPORTS for the library target, which selects dllexport. Consumers
// see dllimport. On ELF/Mach-O we rely on -fvisibility=hidden (set globally in
// CMake) and only export symbols tagged with LIBAGENT_API.

#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef libagent_EXPORTS
#    define LIBAGENT_API __declspec(dllexport)
#  else
#    define LIBAGENT_API __declspec(dllimport)
#  endif
#else
#  if defined(__GNUC__) || defined(__clang__)
#    define LIBAGENT_API __attribute__((visibility("default")))
#  else
#    define LIBAGENT_API
#  endif
#endif
