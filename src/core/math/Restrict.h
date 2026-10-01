#pragma once
// BF_RESTRICT: non-aliasing pointer qualifier for hot DSP loops (lets the compiler vectorise
// without runtime overlap checks). Expands to nothing on unknown compilers.
#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
#define BF_RESTRICT __restrict
#else
#define BF_RESTRICT
#endif
