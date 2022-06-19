// Copyright 2020 Google LLC
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef HIGHWAY_HWY_CACHE_CONTROL_H_
#define HIGHWAY_HWY_CACHE_CONTROL_H_

#include <stddef.h>
#include <stdint.h>

#include "hwy/base.h"

// Requires SSE2; fails to compile on 32-bit Clang 7 (see
// https://github.com/gperftools/gperftools/issues/946).
#if !defined(__SSE2__) || (HWY_COMPILER_CLANG && HWY_ARCH_X86_32)
#undef HWY_DISABLE_CACHE_CONTROL
#define HWY_DISABLE_CACHE_CONTROL
#endif

// intrin.h is sufficient on MSVC and already included by base.h.
#if HWY_ARCH_X86 && !defined(HWY_DISABLE_CACHE_CONTROL) && !HWY_COMPILER_MSVC
#include <emmintrin.h>  // SSE2
#endif

// Windows.h #defines these, which causes infinite recursion. Temporarily
// undefine them in this header; these functions are anyway deprecated.
// TODO(janwas): remove when these functions are removed.
#pragma push_macro("LoadFence")
#undef LoadFence

namespace hwy {

// Even if N*sizeof(T) is smaller, Stream may write a multiple of this size.
#define HWY_STREAM_MULTIPLE 16

// The following functions may also require an attribute.
#if HWY_ARCH_X86 && !defined(HWY_DISABLE_CACHE_CONTROL) && !HWY_COMPILER_MSVC
#define HWY_ATTR_CACHE __attribute__((target("sse2")))
#else
#define HWY_ATTR_CACHE
#endif

// Delays subsequent loads until prior loads are visible. On Intel CPUs, also
// serves as a full fence (waits for all prior instructions to complete).
// No effect on non-x86.
// DEPRECATED due to differing behavior across architectures AND vendors.
HWY_INLINE HWY_ATTR_CACHE void LoadFence() {
#if HWY_ARCH_X86 && !defined(HWY_DISABLE_CACHE_CONTROL)
  _mm_lfence();
#endif
}

// Ensures values written by previous `Stream` calls are visible on the current
// core. This is NOT sufficient for synchronizing across cores; when `Stream`
// outputs are to be consumed by other core(s), the producer must publish
// availability (e.g. via mutex or atomic_flag) after `FlushStream`.
HWY_INLINE HWY_ATTR_CACHE void FlushStream() {
#if HWY_ARCH_X86 && !defined(HWY_DISABLE_CACHE_CONTROL)
  _mm_sfence();
#endif
}

// Optionally begins loading the cache line containing "p" to reduce latency of
// subsequent actual loads.
template <typename T>
HWY_INLINE HWY_ATTR_CACHE void Prefetch(const T* p) {
#if HWY_ARCH_X86 && !defined(HWY_DISABLE_CACHE_CONTROL)
  _mm_prefetch(reinterpret_cast<const char*>(p), _MM_HINT_T0);
#elif HWY_COMPILER_GCC || HWY_COMPILER_CLANG
  // Hint=0 (NTA) behavior differs, but skipping outer caches is probably not
  // desirable, so use the default 3 (keep in caches).
  __builtin_prefetch(p, /*write=*/0, /*hint=*/3);
#else
  (void)p;
#endif
}

// Invalidates and flushes the cache line containing "p", if possible.
HWY_INLINE HWY_ATTR_CACHE void FlushCacheline(const void* p) {
#if HWY_ARCH_X86 && !defined(HWY_DISABLE_CACHE_CONTROL)
  _mm_clflush(p);
#else
  (void)p;
#endif
}

// When called inside a spin-loop, may reduce power consumption.
HWY_INLINE HWY_ATTR_CACHE void Pause() {
#if HWY_ARCH_X86 && !defined(HWY_DISABLE_CACHE_CONTROL)
  _mm_pause();
#endif
}

}  // namespace hwy

// TODO(janwas): remove when these functions are removed. (See above.)
#pragma pop_macro("LoadFence")

#endif  // HIGHWAY_HWY_CACHE_CONTROL_H_
