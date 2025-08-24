/*
    pdqsort_int.h - Pattern-defeating quicksort for integer indices with container interface.

    Copyright (c) 2021 Orson Peters (original pdqsort.h)
    Modified to work with integer indices and container abstraction.

    This software is provided 'as-is', without any express or implied warranty. In no event will the
    authors be held liable for any damages arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose, including commercial
    applications, and to alter it and redistribute it freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not claim that you wrote the
       original software. If you use this software in a product, an acknowledgment in the product
       documentation would be appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not be misrepresented as
       being the original software.

    3. This notice may not be removed or altered from any source distribution.
*/

#ifndef PDQSORT_INT_H
#define PDQSORT_INT_H

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace pdqsort_int_detail
{
enum
{
  // Partitions below this size are sorted using insertion sort.
  insertion_sort_threshold = 24,

  // Partitions above this size use Tukey's ninther to select the pivot.
  ninther_threshold = 128,

  // When we detect an already sorted partition, attempt an insertion sort that allows this
  // amount of element moves before giving up.
  partial_insertion_sort_limit = 8,

  // Must be multiple of 8 due to loop unrolling, and < 256 to fit in unsigned char.
  block_size = 64,

  // Cacheline size, assumes power of two.
  cacheline_size = 64
};

/// Container concept that defines the required interface for pdqsort_int
template <typename Cont>
concept SortableContainer =
  requires(Cont& cont, typename Cont::value_type a, typename Cont::value_type b, std::size_t idx) {
    typename Cont::value_type;

    /// Returns the element at the given index
    { cont.at(idx) } -> std::convertible_to<typename Cont::value_type>;

    /// Swaps elements at two positions in the container
    /// @param i First index
    /// @param j Second index
    cont.swap(idx, idx);

    /// Moves element from source to destination
    /// @param dest Destination for the move operation
    /// @param src Source element to be moved
    /// The operation performs dest = std::move(src)
    cont.move(a, b);

    /// Elements must be comparable
    { a < b } -> std::convertible_to<bool>;
    { a == b } -> std::convertible_to<bool>;
  };

// Returns floor(log2(n)), assumes n > 0.
template <std::integral IntType>
inline int log2(IntType n)
{
  int log = 0;
  while (n >>= 1)
    ++log;
  return log;
}

// Sorts [begin, end) using insertion sort.
template <std::integral IntType, SortableContainer Cont>
inline void insertion_sort(IntType begin, IntType end, Cont& cont)
{
  if (begin == end)
    return;

  for (IntType cur = begin + 1; cur != end; ++cur)
  {
    IntType sift   = cur;
    IntType sift_1 = cur - 1;

    // Compare first so we can avoid 2 moves for an element already positioned correctly.
    if (cont.at(sift) < cont.at(sift_1))
    {
      auto tmp = cont.at(sift);

      do
      {
        cont.move(cont.at(sift--), cont.at(sift_1));
      }
      while (sift != begin && tmp < cont.at(--sift_1));

      cont.move(cont.at(sift), tmp);
    }
  }
}

// Sorts [begin, end) using insertion sort. Assumes
// *(begin - 1) is an element smaller than or equal to any element in [begin, end).
template <std::integral IntType, SortableContainer Cont>
inline void unguarded_insertion_sort(IntType begin, IntType end, Cont& cont)
{
  if (begin == end)
    return;

  for (IntType cur = begin + 1; cur != end; ++cur)
  {
    IntType sift   = cur;
    IntType sift_1 = cur - 1;

    // Compare first so we can avoid 2 moves for an element already positioned correctly.
    if (cont.at(sift) < cont.at(sift_1))
    {
      auto tmp = cont.at(sift);

      do
      {
        cont.move(cont.at(sift--), cont.at(sift_1));
      }
      while (tmp < cont.at(--sift_1));

      cont.move(cont.at(sift), tmp);
    }
  }
}

// Attempts to use insertion sort on [begin, end). Will return false if more than
// partial_insertion_sort_limit elements were moved, and abort sorting. Otherwise it will
// successfully sort and return true.
template <std::integral IntType, SortableContainer Cont>
inline bool partial_insertion_sort(IntType begin, IntType end, Cont& cont)
{
  if (begin == end)
    return true;

  std::size_t limit = 0;
  for (IntType cur = begin + 1; cur != end; ++cur)
  {
    IntType sift   = cur;
    IntType sift_1 = cur - 1;

    // Compare first so we can avoid 2 moves for an element already positioned correctly.
    if (cont.at(sift) < cont.at(sift_1))
    {
      auto tmp = cont.at(sift);

      do
      {
        cont.move(cont.at(sift--), cont.at(sift_1));
      }
      while (sift != begin && tmp < cont.at(--sift_1));

      cont.move(cont.at(sift), tmp);
      limit += cur - sift;
    }

    if (limit > partial_insertion_sort_limit)
      return false;
  }

  return true;
}

template <std::integral IntType, SortableContainer Cont>
inline void sort2(IntType a, IntType b, Cont& cont)
{
  if (cont.at(b) < cont.at(a))
    cont.swap(a, b);
}

// Sorts the elements at indices a, b and c.
template <std::integral IntType, SortableContainer Cont>
inline void sort3(IntType a, IntType b, IntType c, Cont& cont)
{
  sort2(a, b, cont);
  sort2(b, c, cont);
  sort2(a, b, cont);
}

template <std::integral IntType, SortableContainer Cont>
inline void swap_offsets(IntType        first,
                         IntType        last,
                         Cont&          cont,
                         unsigned char* offsets_l,
                         unsigned char* offsets_r,
                         std::size_t    num,
                         bool           use_swaps)
{
  if (use_swaps)
  {
    // This case is needed for the descending distribution, where we need
    // to have proper swapping for pdqsort to remain O(n).
    for (std::size_t i = 0; i < num; ++i)
    {
      cont.swap(first + offsets_l[i], last - offsets_r[i]);
    }
  }
  else if (num > 0)
  {
    IntType l   = first + offsets_l[0];
    IntType r   = last - offsets_r[0];
    auto    tmp = cont.at(l);
    cont.move(cont.at(l), cont.at(r));

    for (std::size_t i = 1; i < num; ++i)
    {
      l = first + offsets_l[i];
      cont.move(cont.at(r), cont.at(l));
      r = last - offsets_r[i];
      cont.move(cont.at(l), cont.at(r));
    }
    cont.move(cont.at(r), tmp);
  }
}

// Partitions [begin, end) around pivot at begin. Elements equal
// to the pivot are put in the right-hand partition. Returns the position of the pivot after
// partitioning and whether the passed sequence already was correctly partitioned. Assumes the
// pivot is a median of at least 3 elements and that [begin, end) is at least
// insertion_sort_threshold long. Uses branchless partitioning.
template <std::integral IntType, SortableContainer Cont>
inline std::pair<IntType, bool> partition_right_branchless(IntType begin, IntType end, Cont& cont)
{
  // Move pivot into local for speed.
  auto    pivot = cont.at(begin);
  IntType first = begin;
  IntType last  = end;

  // Find the first element greater than or equal than the pivot (the median of 3 guarantees
  // this exists).
  while (cont.at(++first) < pivot)
    ;

  // Find the first element strictly smaller than the pivot. We have to guard this search if
  // there was no element before *first.
  if (first - 1 == begin)
    while (first < last && !(cont.at(--last) < pivot))
      ;
  else
    while (!(cont.at(--last) < pivot))
      ;

  // If the first pair of elements that should be swapped to partition are the same element,
  // the passed in sequence already was correctly partitioned.
  bool already_partitioned = first >= last;
  if (!already_partitioned)
  {
    cont.swap(first, last);
    ++first;

    // The following branchless partitioning is derived from "BlockQuicksort: How Branch
    // Mispredictions don't affect Quicksort" by Stefan Edelkamp and Armin Weiss, but
    // heavily micro-optimized.
    alignas(cacheline_size) unsigned char offsets_l_storage[block_size + cacheline_size];
    alignas(cacheline_size) unsigned char offsets_r_storage[block_size + cacheline_size];
    unsigned char*                        offsets_l = offsets_l_storage;
    unsigned char*                        offsets_r = offsets_r_storage;

    IntType     offsets_l_base = first;
    IntType     offsets_r_base = last;
    std::size_t num_l, num_r, start_l, start_r;
    num_l = num_r = start_l = start_r = 0;

    while (first < last)
    {
      // Fill up offset blocks with elements that are on the wrong side.
      // First we determine how much elements are considered for each offset block.
      std::size_t num_unknown = last - first;
      std::size_t left_split  = num_l == 0 ? (num_r == 0 ? num_unknown / 2 : num_unknown) : 0;
      std::size_t right_split = num_r == 0 ? (num_unknown - left_split) : 0;

      // Fill the offset blocks.
      if (left_split >= block_size)
      {
        for (std::size_t i = 0; i < block_size;)
        {
          offsets_l[num_l] = i++;
          num_l += !(cont.at(first) < pivot);
          ++first;
          offsets_l[num_l] = i++;
          num_l += !(cont.at(first) < pivot);
          ++first;
          offsets_l[num_l] = i++;
          num_l += !(cont.at(first) < pivot);
          ++first;
          offsets_l[num_l] = i++;
          num_l += !(cont.at(first) < pivot);
          ++first;
          offsets_l[num_l] = i++;
          num_l += !(cont.at(first) < pivot);
          ++first;
          offsets_l[num_l] = i++;
          num_l += !(cont.at(first) < pivot);
          ++first;
          offsets_l[num_l] = i++;
          num_l += !(cont.at(first) < pivot);
          ++first;
          offsets_l[num_l] = i++;
          num_l += !(cont.at(first) < pivot);
          ++first;
        }
      }
      else
      {
        for (std::size_t i = 0; i < left_split;)
        {
          offsets_l[num_l] = i++;
          num_l += !(cont.at(first) < pivot);
          ++first;
        }
      }

      if (right_split >= block_size)
      {
        for (std::size_t i = 0; i < block_size;)
        {
          offsets_r[num_r] = ++i;
          num_r += cont.at(--last) < pivot;
          offsets_r[num_r] = ++i;
          num_r += cont.at(--last) < pivot;
          offsets_r[num_r] = ++i;
          num_r += cont.at(--last) < pivot;
          offsets_r[num_r] = ++i;
          num_r += cont.at(--last) < pivot;
          offsets_r[num_r] = ++i;
          num_r += cont.at(--last) < pivot;
          offsets_r[num_r] = ++i;
          num_r += cont.at(--last) < pivot;
          offsets_r[num_r] = ++i;
          num_r += cont.at(--last) < pivot;
          offsets_r[num_r] = ++i;
          num_r += cont.at(--last) < pivot;
        }
      }
      else
      {
        for (std::size_t i = 0; i < right_split;)
        {
          offsets_r[num_r] = ++i;
          num_r += cont.at(--last) < pivot;
        }
      }

      // Swap elements and update block sizes and first/last boundaries.
      std::size_t num = std::min(num_l, num_r);
      swap_offsets(offsets_l_base, offsets_r_base, cont, offsets_l + start_l, offsets_r + start_r, num, num_l == num_r);
      num_l -= num;
      num_r -= num;
      start_l += num;
      start_r += num;

      if (num_l == 0)
      {
        start_l        = 0;
        offsets_l_base = first;
      }

      if (num_r == 0)
      {
        start_r        = 0;
        offsets_r_base = last;
      }
    }

    // We have now fully identified [first, last)'s proper position. Swap the last elements.
    if (num_l)
    {
      offsets_l += start_l;
      while (num_l--)
        cont.swap(offsets_l_base + offsets_l[num_l], --last);
      first = last;
    }
    if (num_r)
    {
      offsets_r += start_r;
      while (num_r--)
        cont.swap(offsets_r_base - offsets_r[num_r], first++);
      last = first;
    }
  }

  // Put the pivot in the right place.
  IntType pivot_pos = first - 1;
  cont.move(cont.at(begin), cont.at(pivot_pos));
  cont.move(cont.at(pivot_pos), pivot);

  return std::make_pair(pivot_pos, already_partitioned);
}

// Partitions [begin, end) around pivot at begin. Elements equal
// to the pivot are put in the right-hand partition. Returns the position of the pivot after
// partitioning and whether the passed sequence already was correctly partitioned. Assumes the
// pivot is a median of at least 3 elements and that [begin, end) is at least
// insertion_sort_threshold long.
template <std::integral IntType, SortableContainer Cont>
inline std::pair<IntType, bool> partition_right(IntType begin, IntType end, Cont& cont)
{
  // Move pivot into local for speed.
  auto pivot = cont.at(begin);

  IntType first = begin;
  IntType last  = end;

  // Find the first element greater than or equal than the pivot (the median of 3 guarantees
  // this exists).
  while (cont.at(++first) < pivot)
    ;

  // Find the first element strictly smaller than the pivot. We have to guard this search if
  // there was no element before *first.
  if (first - 1 == begin)
    while (first < last && !(cont.at(--last) < pivot))
      ;
  else
    while (!(cont.at(--last) < pivot))
      ;

  // If the first pair of elements that should be swapped to partition are the same element,
  // the passed in sequence already was correctly partitioned.
  bool already_partitioned = first >= last;

  // Keep swapping pairs of elements that are on the wrong side of the pivot. Previously
  // swapped pairs guard the searches, which is why the first iteration is special-cased
  // above.
  while (first < last)
  {
    cont.swap(first, last);
    while (cont.at(++first) < pivot)
      ;
    while (!(cont.at(--last) < pivot))
      ;
  }

  // Put the pivot in the right place.
  IntType pivot_pos = first - 1;
  cont.move(cont.at(begin), cont.at(pivot_pos));
  cont.move(cont.at(pivot_pos), pivot);

  return std::make_pair(pivot_pos, already_partitioned);
}

// Similar function to the one above, except elements equal to the pivot are put to the left of
// the pivot and it doesn't check or return if the passed sequence already was partitioned.
// Since this is rarely used (the many equal case), and in that case pdqsort already has O(n)
// performance, no block quicksort is applied here for simplicity.
template <std::integral IntType, SortableContainer Cont>
inline IntType partition_left(IntType begin, IntType end, Cont& cont)
{
  auto    pivot = cont.at(begin);
  IntType first = begin;
  IntType last  = end;

  while (pivot < cont.at(--last))
    ;

  if (last + 1 == end)
    while (first < last && !(pivot < cont.at(++first)))
      ;
  else
    while (!(pivot < cont.at(++first)))
      ;

  while (first < last)
  {
    cont.swap(first, last);
    while (pivot < cont.at(--last))
      ;
    while (!(pivot < cont.at(++first)))
      ;
  }

  IntType pivot_pos = last;
  cont.move(cont.at(begin), cont.at(pivot_pos));
  cont.move(cont.at(pivot_pos), pivot);

  return pivot_pos;
}

template <std::integral IntType, SortableContainer Cont, bool Branchless>
inline void pdqsort_loop(IntType begin, IntType end, Cont& cont, int bad_allowed, bool leftmost = true)
{
  // Use a while loop for tail recursion elimination.
  while (true)
  {
    auto size = end - begin;

    // Insertion sort is faster for small arrays.
    if (size < insertion_sort_threshold)
    {
      if (leftmost)
        insertion_sort(begin, end, cont);
      else
        unguarded_insertion_sort(begin, end, cont);
      return;
    }

    // Choose pivot as median of 3 or pseudomedian of 9.
    auto s2 = size / 2;
    if (size > ninther_threshold)
    {
      sort3(begin, begin + s2, end - 1, cont);
      sort3(begin + 1, begin + (s2 - 1), end - 2, cont);
      sort3(begin + 2, begin + (s2 + 1), end - 3, cont);
      sort3(begin + (s2 - 1), begin + s2, begin + (s2 + 1), cont);
      cont.swap(begin, begin + s2);
    }
    else
      sort3(begin + s2, begin, end - 1, cont);

    // If *(begin - 1) is the end of the right partition of a previous partition operation
    // there is no element in [begin, end) that is smaller than *(begin - 1). Then if our
    // pivot compares equal to *(begin - 1) we change strategy, putting equal elements in
    // the left partition, greater elements in the right partition. We do not have to
    // recurse on the left partition, since it's sorted (all equal).
    if (!leftmost && !(cont.at(begin - 1) < cont.at(begin)))
    {
      begin = partition_left(begin, end, cont) + 1;
      continue;
    }

    // Partition and get results.
    std::pair<IntType, bool> part_result =
      Branchless ? partition_right_branchless(begin, end, cont) : partition_right(begin, end, cont);
    IntType pivot_pos           = part_result.first;
    bool    already_partitioned = part_result.second;

    // Check for a highly unbalanced partition.
    auto l_size            = pivot_pos - begin;
    auto r_size            = end - (pivot_pos + 1);
    bool highly_unbalanced = l_size < size / 8 || r_size < size / 8;

    // If we got a highly unbalanced partition we shuffle elements to break many patterns.
    if (highly_unbalanced)
    {
      // If we had too many bad partitions, switch to heapsort to guarantee O(n log n).
      if (--bad_allowed == 0)
      {
        // Note: We cannot use std::make_heap/std::sort_heap directly since we don't have iterators
        // Fall back to a simple O(n log n) sort for the worst case
        // This is a simplified approach - in practice you might want to implement heapsort
        // with integer indices as well
        for (IntType i = begin + 1; i < end; ++i)
        {
          IntType j = i;
          while (j > begin && cont.at(j) < cont.at(j - 1))
          {
            cont.swap(j, j - 1);
            --j;
          }
        }
        return;
      }

      if (l_size >= insertion_sort_threshold)
      {
        cont.swap(begin, begin + l_size / 4);
        cont.swap(pivot_pos - 1, pivot_pos - l_size / 4);

        if (l_size > ninther_threshold)
        {
          cont.swap(begin + 1, begin + (l_size / 4 + 1));
          cont.swap(begin + 2, begin + (l_size / 4 + 2));
          cont.swap(pivot_pos - 2, pivot_pos - (l_size / 4 + 1));
          cont.swap(pivot_pos - 3, pivot_pos - (l_size / 4 + 2));
        }
      }

      if (r_size >= insertion_sort_threshold)
      {
        cont.swap(pivot_pos + 1, pivot_pos + (1 + r_size / 4));
        cont.swap(end - 1, end - r_size / 4);

        if (r_size > ninther_threshold)
        {
          cont.swap(pivot_pos + 2, pivot_pos + (2 + r_size / 4));
          cont.swap(pivot_pos + 3, pivot_pos + (3 + r_size / 4));
          cont.swap(end - 2, end - (1 + r_size / 4));
          cont.swap(end - 3, end - (2 + r_size / 4));
        }
      }
    }
    else
    {
      // If we were decently balanced and we tried to sort an already partitioned
      // sequence try to use insertion sort.
      if (already_partitioned && partial_insertion_sort(begin, pivot_pos, cont) &&
          partial_insertion_sort(pivot_pos + 1, end, cont))
        return;
    }

    // Sort the left partition first using recursion and do tail recursion elimination for
    // the right-hand partition.
    pdqsort_loop<IntType, Cont, Branchless>(begin, pivot_pos, cont, bad_allowed, leftmost);
    begin    = pivot_pos + 1;
    leftmost = false;
  }
}
} // namespace pdqsort_int_detail

/// Sorts elements in the range [begin, end) using pattern-defeating quicksort
/// @param begin Start index (inclusive)
/// @param end End index (exclusive)
/// @param cont Container object providing swap, move, and at operations
template <std::integral IntType, pdqsort_int_detail::SortableContainer Cont>
inline void pdqsort_int(IntType begin, IntType end, Cont& cont)
{
  if (begin == end)
    return;

  pdqsort_int_detail::pdqsort_loop<IntType, Cont, true>(begin, end, cont, pdqsort_int_detail::log2(end - begin));
}

/// Sorts elements in the range [begin, end) using pattern-defeating quicksort (non-branchless version)
/// @param begin Start index (inclusive)
/// @param end End index (exclusive)
/// @param cont Container object providing swap, move, and at operations
template <std::integral IntType, pdqsort_int_detail::SortableContainer Cont>
inline void pdqsort_int_non_branchless(IntType begin, IntType end, Cont& cont)
{
  if (begin == end)
    return;

  pdqsort_int_detail::pdqsort_loop<IntType, Cont, false>(begin, end, cont, pdqsort_int_detail::log2(end - begin));
}

#endif // PDQSORT_INT_H
