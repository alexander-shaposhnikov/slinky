#include "buffer.h"

#include <cstdint>

namespace slinky {

namespace {

struct copy_dim {
  index_t pad_before;
  index_t size;
  index_t pad_after;
  index_t total_size;
  index_t src_stride;
  index_t dst_stride;

  bool operator<(const copy_dim& r) const { return dst_stride < r.dst_stride; }
};

template <typename T>
void fill(T* dst, T value, index_t size) {
  std::fill(dst, dst + size, value);
}

void fill(char* dst, index_t elem_size, const void* value, index_t size) {
  if (!value) return;

  switch (elem_size) {
  case 1: fill(reinterpret_cast<uint8_t*>(dst), *reinterpret_cast<const uint8_t*>(value), size); return;
  case 2: fill(reinterpret_cast<uint16_t*>(dst), *reinterpret_cast<const uint16_t*>(value), size); return;
  case 4: fill(reinterpret_cast<uint32_t*>(dst), *reinterpret_cast<const uint32_t*>(value), size); return;
  case 8: fill(reinterpret_cast<uint64_t*>(dst), *reinterpret_cast<const uint64_t*>(value), size); return;
  }
  for (index_t i = 0; i < size; ++i) {
    memcpy(dst, value, elem_size);
    dst += elem_size;
  }
}

void fill(char* dst, index_t stride, index_t elem_size, const void* value, index_t size) {
  if (!value) return;

  for (index_t i = 0; i < size; ++i) {
    memcpy(dst, value, elem_size);
    dst += stride;
  }
}

void copy(const char* src, index_t src_stride, char* dst, index_t dst_stride, index_t elem_size, index_t size) {
  for (index_t i = 0; i < size; ++i) {
    memcpy(dst, src, elem_size);
    dst += dst_stride;
    src += src_stride;
  }
}

void fill(char* dst, const copy_dim* dims, index_t elem_size, const void* value, int dim) {
  if (!value) return;

  const copy_dim& d = dims[dim];
  if (dim == 0) {
    if (d.dst_stride == elem_size) {
      fill(dst, elem_size, value, d.total_size);
    } else {
      fill(dst, d.dst_stride, elem_size, value, d.total_size);
    }
  } else {
    for (index_t i = 0; i < d.total_size; ++i) {
      fill(dst, dims, elem_size, value, dim - 1);
      dst += d.dst_stride;
    }
  }
}

void copy(const char* src, char* dst, const copy_dim* dims, index_t elem_size, const void* padding, int dim) {
  // src can be nullptr, in which case we should only fill the padding.
  const copy_dim& d = dims[dim];
  if (dim == 0) {
    if (d.dst_stride == elem_size) {
      if (d.pad_before > 0) {
        fill(dst, elem_size, padding, d.pad_before);
        dst += d.pad_before * d.dst_stride;
      }
      if (d.src_stride == elem_size) {
        // src and dst are both dense, this can be implemented by memcpy.
        if (src) memcpy(dst, src, d.size * elem_size);
        dst += d.size * elem_size;
      } else if (d.src_stride == 0) {
        // Special case for broadcasting to a dense dst.
        if (src) fill(dst, elem_size, src, d.size);
        dst += d.size * elem_size;
      } else {
        // Need to copy one element at a time to skip padding.
        if (src) copy(src, d.src_stride, dst, d.dst_stride, elem_size, d.size);
        dst += d.size * d.dst_stride;
      }
      if (d.pad_after > 0) {
        fill(dst, elem_size, padding, d.pad_after);
      }
    } else {
      // Need to copy one element at a time to skip padding.
      if (d.pad_before > 0) {
        fill(dst, d.dst_stride, elem_size, padding, d.pad_before);
        dst += d.dst_stride * d.pad_before;
      }
      if (src) copy(src, d.src_stride, dst, d.dst_stride, elem_size, d.size);
      dst += d.size * d.dst_stride;
      if (d.pad_after > 0) {
        fill(dst, d.dst_stride, elem_size, padding, d.pad_after);
      }
    }
  } else {
    for (index_t i = 0; i < d.pad_before; ++i) {
      fill(dst, dims, elem_size, padding, dim - 1);
      dst += d.dst_stride;
    }
    for (index_t i = 0; i < d.size; ++i) {
      copy(src, dst, dims, elem_size, padding, dim - 1);
      if (src) src += d.src_stride;
      dst += d.dst_stride;
    }
    for (index_t i = 0; i < d.pad_after; ++i) {
      fill(dst, dims, elem_size, padding, dim - 1);
      dst += d.dst_stride;
    }
  }
}

// For sorting tiny arrays of dimension metadata, this is faster than std::sort.
template <class It>
void bubble_sort(It begin, It end) {
  for (It i = begin; i != end; ++i) {
    for (It j = i; j != end; ++j) {
      if (*j < *i) {
        std::swap(*i, *j);
      }
    }
  }
}

index_t compute_padding(const dim& src, const dim& dst, copy_dim& dim) {
  index_t src_offset = 0;
  if (dst.end() <= src.begin() || dst.begin() >= src.end()) {
    // This dimension is all padding.
    dim.pad_before = dim.total_size;
    dim.size = 0;
    dim.pad_after = 0;
  } else {
    index_t copy_begin = std::max(src.begin(), dst.begin());
    index_t copy_end = std::min(src.end(), dst.end());
    dim.size = std::max<index_t>(0, copy_end - copy_begin);
    dim.pad_before = std::max<index_t>(0, copy_begin - dst.begin());
    dim.pad_after = std::max<index_t>(0, dst.end() - copy_end);

    // If the src min is before the dst min, adjust the base.
    if (dst.begin() > src.begin()) {
      src_offset = dim.src_stride * (dst.begin() - src.begin());
    }
  }

  assert(dim.pad_before + dim.pad_after + dim.size == dim.total_size);
  return src_offset;
}

std::size_t optimize_copy_dims(copy_dim* dims, std::size_t rank) {
  if (rank == 1) return 1;

  // Sort the dims by (dst) stride.
  bubble_sort(dims, dims + rank);

  // Find dimensions we can fuse.
  for (std::size_t d = 0; d + 1 < rank;) {
    copy_dim& a = dims[d];
    const copy_dim& b = dims[d + 1];
    if (a.pad_before == 0 && a.pad_after == 0 &&
        b.dst_stride == a.dst_stride * a.total_size &&
        b.src_stride == a.src_stride * a.total_size) {
      assert(a.size == a.total_size);
      a.pad_before = b.pad_before * a.size;
      a.pad_after = b.pad_after * a.size;
      a.total_size = b.total_size * a.size;
      a.size = b.size * a.size;

      // Remove the now-fused dimension.
      for (std::size_t i = d + 1; i + 1 < rank; ++i) {
        dims[i] = dims[i + 1];
      }
      --rank;
    } else {
      ++d;
    }
  }
  return rank;
}

}  // namespace

void copy(const raw_buffer& src, const raw_buffer& dst, const void* padding) {
  assert(src.rank == dst.rank);
  assert(src.elem_size == dst.elem_size);

  std::size_t rank = dst.rank;
  if (rank == 0) {
    // The buffers are scalar.
    memcpy(dst.base, src.base, dst.elem_size);
    return;
  }

  const char* src_base = reinterpret_cast<const char*>(src.base);
  char* dst_base = reinterpret_cast<char*>(dst.base);

  // Make a list of pointers to dims that we are going to copy.
  copy_dim* dims = reinterpret_cast<copy_dim*>(alloca(sizeof(copy_dim) * rank));
  for (std::size_t i = 0; i < rank; ++i) {
    dims[i].src_stride = src.dims[i].stride();
    dims[i].dst_stride = dst.dims[i].stride();
    dims[i].total_size = dst.dims[i].extent();
    src_base += compute_padding(src.dims[i], dst.dims[i], dims[i]);
  }

  rank = optimize_copy_dims(dims, rank);

  // Now we have an optimized set of dimensions to copy. Run the copy.
  copy(src_base, dst_base, dims, dst.elem_size, padding, rank - 1);
}

void pad(const dim* in_bounds, const raw_buffer& dst, const void* padding) {
  std::size_t rank = dst.rank;
  if (rank == 0) {
    // The buffer is scalar.
    return;
  }

  char* dst_base = reinterpret_cast<char*>(dst.base);

  // Make a list of pointers to dims that we are going to pad.
  copy_dim* dims = reinterpret_cast<copy_dim*>(alloca(sizeof(copy_dim) * rank));
  for (std::size_t i = 0; i < rank; ++i) {
    dims[i].src_stride = 0;
    dims[i].dst_stride = dst.dims[i].stride();
    dims[i].total_size = dst.dims[i].extent();
    compute_padding(in_bounds[i], dst.dims[i], dims[i]);
  }

  rank = optimize_copy_dims(dims, rank);

  // Now we have an optimized set of dimensions to pad. Run the pad.
  copy(nullptr, dst_base, dims, dst.elem_size, padding, rank - 1);
}

void fill(const raw_buffer& dst, const void* value) {
  std::size_t rank = dst.rank;
  if (rank == 0) {
    // The buffer is scalar.
    memcpy(dst.base, value, dst.elem_size);
    return;
  }

  char* dst_base = reinterpret_cast<char*>(dst.base);

  // Make a list of pointers to dims that we are going to copy.
  copy_dim* dims = reinterpret_cast<copy_dim*>(alloca(sizeof(copy_dim) * rank));
  for (std::size_t i = 0; i < rank; ++i) {
    dims[i].dst_stride = dst.dim(i).stride();
    dims[i].src_stride = 0;  // For optimize_copy_dims
    dims[i].total_size = dst.dim(i).extent();

    dims[i].pad_before = dims[i].total_size;
    dims[i].size = 0;
    dims[i].pad_after = 0;
  }

  rank = optimize_copy_dims(dims, rank);

  fill(dst_base, dims, dst.elem_size, value, rank - 1);
}

}  // namespace slinky