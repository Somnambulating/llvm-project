//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___ALGORITHM_PSTL_BACKENDS_CPU_BACKENDS_LIBDISPATCH_H
#define _LIBCPP___ALGORITHM_PSTL_BACKENDS_CPU_BACKENDS_LIBDISPATCH_H

#include <__algorithm/lower_bound.h>
#include <__algorithm/max.h>
#include <__algorithm/upper_bound.h>
#include <__atomic/atomic.h>
#include <__config>
#include <__exception/terminate.h>
#include <__iterator/iterator_traits.h>
#include <__iterator/move_iterator.h>
#include <__memory/allocator.h>
#include <__memory/construct_at.h>
#include <__memory/unique_ptr.h>
#include <__numeric/reduce.h>
#include <__utility/exception_guard.h>
#include <__utility/move.h>
#include <__utility/pair.h>
#include <__utility/terminate_on_exception.h>
#include <cstddef>
#include <new>

_LIBCPP_PUSH_MACROS
#include <__undef_macros>

#if !defined(_LIBCPP_HAS_NO_INCOMPLETE_PSTL) && _LIBCPP_STD_VER >= 17

_LIBCPP_BEGIN_NAMESPACE_STD

namespace __par_backend {
inline namespace __libdispatch {

// ::dispatch_apply is marked as __attribute__((nothrow)) because it doesn't let exceptions propagate, and neither do
// we.
// TODO: Do we want to add [[_Clang::__callback__(__func, __context, __)]]?
_LIBCPP_EXPORTED_FROM_ABI void
__dispatch_apply(size_t __chunk_count, void* __context, void (*__func)(void* __context, size_t __chunk)) noexcept;

template <class _Func>
_LIBCPP_HIDE_FROM_ABI void __dispatch_apply(size_t __chunk_count, _Func __func) noexcept {
  __libdispatch::__dispatch_apply(__chunk_count, &__func, [](void* __context, size_t __chunk) {
    (*static_cast<_Func*>(__context))(__chunk);
  });
}

struct __chunk_partitions {
  ptrdiff_t __chunk_count_; // includes the first chunk
  ptrdiff_t __chunk_size_;
  ptrdiff_t __first_chunk_size_;
};

[[__gnu__::__const__]] _LIBCPP_EXPORTED_FROM_ABI __chunk_partitions __partition_chunks(ptrdiff_t __size) noexcept;

template <class _RandomAccessIterator, class _Functor>
_LIBCPP_HIDE_FROM_ABI void
__parallel_for(_RandomAccessIterator __first, _RandomAccessIterator __last, _Functor __func) {
  auto __partitions = __libdispatch::__partition_chunks(__last - __first);

  // Perform the chunked execution.
  __libdispatch::__dispatch_apply(__partitions.__chunk_count_, [&](size_t __chunk) {
    auto __this_chunk_size = __chunk == 0 ? __partitions.__first_chunk_size_ : __partitions.__chunk_size_;
    auto __index =
        __chunk == 0
            ? 0
            : (__chunk * __partitions.__chunk_size_) + (__partitions.__first_chunk_size_ - __partitions.__chunk_size_);
    __func(__first + __index, __first + __index + __this_chunk_size);
  });
}

template <class _RandomAccessIterator1, class _RandomAccessIterator2, class _RandomAccessIteratorOut>
struct __merge_range {
  __merge_range(_RandomAccessIterator1 __mid1, _RandomAccessIterator2 __mid2, _RandomAccessIteratorOut __result)
      : __mid1_(__mid1), __mid2_(__mid2), __result_(__result) {}

  _RandomAccessIterator1 __mid1_;
  _RandomAccessIterator2 __mid2_;
  _RandomAccessIteratorOut __result_;
};

template <typename _RandomAccessIterator1,
          typename _RandomAccessIterator2,
          typename _RandomAccessIterator3,
          typename _Compare,
          typename _LeafMerge>
_LIBCPP_HIDE_FROM_ABI void __parallel_merge(
    _RandomAccessIterator1 __first1,
    _RandomAccessIterator1 __last1,
    _RandomAccessIterator2 __first2,
    _RandomAccessIterator2 __last2,
    _RandomAccessIterator3 __result,
    _Compare __comp,
    _LeafMerge __leaf_merge) {
  __chunk_partitions __partitions =
      __libdispatch::__partition_chunks(std::max<ptrdiff_t>(__last1 - __first1, __last2 - __first2));

  if (__partitions.__chunk_count_ == 0)
    return;

  if (__partitions.__chunk_count_ == 1) {
    __leaf_merge(__first1, __last1, __first2, __last2, __result, __comp);
    return;
  }

  using __merge_range_t = __merge_range<_RandomAccessIterator1, _RandomAccessIterator2, _RandomAccessIterator3>;
  auto const __n_ranges = __partitions.__chunk_count_ + 1;

  // TODO: use __uninitialized_buffer
  auto __destroy = [=](__merge_range_t* __ptr) {
    std::destroy_n(__ptr, __n_ranges);
    std::allocator<__merge_range_t>().deallocate(__ptr, __n_ranges);
  };
  unique_ptr<__merge_range_t[], decltype(__destroy)> __ranges(
      std::allocator<__merge_range_t>().allocate(__n_ranges), __destroy);

  // TODO: Improve the case where the smaller range is merged into just a few (or even one) chunks of the larger case
  std::__terminate_on_exception([&] {
    __merge_range_t* __r = __ranges.get();
    std::__construct_at(__r++, __first1, __first2, __result);

    bool __iterate_first_range = __last1 - __first1 > __last2 - __first2;

    auto __compute_chunk = [&](size_t __chunk_size) -> __merge_range_t {
      auto [__mid1, __mid2] = [&] {
        if (__iterate_first_range) {
          auto __m1 = __first1 + __chunk_size;
          auto __m2 = std::lower_bound(__first2, __last2, __m1[-1], __comp);
          return std::make_pair(__m1, __m2);
        } else {
          auto __m2 = __first2 + __chunk_size;
          auto __m1 = std::lower_bound(__first1, __last1, __m2[-1], __comp);
          return std::make_pair(__m1, __m2);
        }
      }();

      __result += (__mid1 - __first1) + (__mid2 - __first2);
      __first1 = __mid1;
      __first2 = __mid2;
      return {std::move(__mid1), std::move(__mid2), __result};
    };

    // handle first chunk
    std::__construct_at(__r++, __compute_chunk(__partitions.__first_chunk_size_));

    // handle 2 -> N - 1 chunks
    for (ptrdiff_t __i = 0; __i != __partitions.__chunk_count_ - 2; ++__i)
      std::__construct_at(__r++, __compute_chunk(__partitions.__chunk_size_));

    // handle last chunk
    std::__construct_at(__r, __last1, __last2, __result);

    __libdispatch::__dispatch_apply(__partitions.__chunk_count_, [&](size_t __index) {
      auto __first_iters = __ranges[__index];
      auto __last_iters  = __ranges[__index + 1];
      __leaf_merge(
          __first_iters.__mid1_,
          __last_iters.__mid1_,
          __first_iters.__mid2_,
          __last_iters.__mid2_,
          __first_iters.__result_,
          __comp);
    });
  });
}

template <class _RandomAccessIterator, class _Transform, class _Value, class _Combiner, class _Reduction>
_LIBCPP_HIDE_FROM_ABI _Value __parallel_transform_reduce(
    _RandomAccessIterator __first,
    _RandomAccessIterator __last,
    _Transform __transform,
    _Value __init,
    _Combiner __combiner,
    _Reduction __reduction) {
  if (__first == __last)
    return __init;

  auto __partitions = __libdispatch::__partition_chunks(__last - __first);

  auto __destroy = [__count = __partitions.__chunk_count_](_Value* __ptr) {
    std::destroy_n(__ptr, __count);
    std::allocator<_Value>().deallocate(__ptr, __count);
  };

  // TODO: use __uninitialized_buffer
  // TODO: allocate one element per worker instead of one element per chunk
  unique_ptr<_Value[], decltype(__destroy)> __values(
      std::allocator<_Value>().allocate(__partitions.__chunk_count_), __destroy);

  // __dispatch_apply is noexcept
  __libdispatch::__dispatch_apply(__partitions.__chunk_count_, [&](size_t __chunk) {
    auto __this_chunk_size = __chunk == 0 ? __partitions.__first_chunk_size_ : __partitions.__chunk_size_;
    auto __index =
        __chunk == 0
            ? 0
            : (__chunk * __partitions.__chunk_size_) + (__partitions.__first_chunk_size_ - __partitions.__chunk_size_);
    if (__this_chunk_size != 1) {
      std::__construct_at(
          __values.get() + __chunk,
          __reduction(__first + __index + 2,
                      __first + __index + __this_chunk_size,
                      __combiner(__transform(__first + __index), __transform(__first + __index + 1))));
    } else {
      std::__construct_at(__values.get() + __chunk, __transform(__first + __index));
    }
  });

  return std::__terminate_on_exception([&] {
    return std::reduce(
        std::make_move_iterator(__values.get()),
        std::make_move_iterator(__values.get() + __partitions.__chunk_count_),
        std::move(__init),
        __combiner);
  });
}

// TODO: parallelize this
template <class _RandomAccessIterator, class _Comp, class _LeafSort>
_LIBCPP_HIDE_FROM_ABI void __parallel_stable_sort(
    _RandomAccessIterator __first, _RandomAccessIterator __last, _Comp __comp, _LeafSort __leaf_sort) {
  __leaf_sort(__first, __last, __comp);
}

_LIBCPP_HIDE_FROM_ABI inline void __cancel_execution() {}

} // namespace __libdispatch
} // namespace __par_backend

_LIBCPP_END_NAMESPACE_STD

#endif // !defined(_LIBCPP_HAS_NO_INCOMPLETE_PSTL) && _LIBCPP_STD_VER >= 17

_LIBCPP_POP_MACROS

#endif // _LIBCPP___ALGORITHM_PSTL_BACKENDS_CPU_BACKENDS_LIBDISPATCH_H
