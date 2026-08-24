#pragma once

#if defined(_WIN32)
#  if defined(HDCODEX_EXPORTS)
#    define HDCODEX_API __declspec(dllexport)
#  else
#    define HDCODEX_API __declspec(dllimport)
#  endif
#else
#  define HDCODEX_API __attribute__((visibility("default")))
#endif

