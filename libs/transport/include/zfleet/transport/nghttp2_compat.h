#pragma once

#include <cstddef>

#if defined(_MSC_VER) && !defined(_SSIZE_T_DEFINED)
using ssize_t = std::ptrdiff_t;
#define _SSIZE_T_DEFINED
#endif

#include <nghttp2/nghttp2.h>
