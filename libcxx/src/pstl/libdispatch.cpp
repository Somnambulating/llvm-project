//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <__algorithm/min.h>
#include <__algorithm/pstl_backends/cpu_backends/libdispatch.h>
#include <__config>
#include <dispatch/dispatch.h>

_LIBCPP_BEGIN_NAMESPACE_STD

namespace __par_backend::inline __libdispatch {

void __dispatch_apply(size_t chunk_count, void* context, void (*func)(void* context, size_t chunk)) noexcept {
  ::dispatch_apply_f(chunk_count, DISPATCH_APPLY_AUTO, context, func);
}

__chunk_partitions __partition_chunks(ptrdiff_t element_count) noexcept {
  __chunk_partitions partitions;
  partitions.__chunk_count_      = std::max<ptrdiff_t>(1, element_count / 256);
  partitions.__chunk_size_       = element_count / partitions.__chunk_count_;
  partitions.__first_chunk_size_ = element_count - (partitions.__chunk_count_ - 1) * partitions.__chunk_size_;
  return partitions;
}

// NOLINTNEXTLINE(llvm-namespace-comment) // This is https://llvm.org/PR56804
} // namespace __par_backend::inline __libdispatch

_LIBCPP_END_NAMESPACE_STD
