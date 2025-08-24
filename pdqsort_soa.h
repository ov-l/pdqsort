/*
    pdqsort_soa.h - Pattern‑defeating quicksort for structure‑of‑arrays and
   array‑of‑structures.

    This header provides an implementation of the pattern‑defeating quicksort
   (pdqsort) algorithm specialised for sorting containers that hold their data
   in a structure‑of‑arrays (SoA) layout. Instead of accepting iterators and a
   comparison function like the regular pdqsort, this variant operates on a
   contiguous integer index range and a user supplied container instance. The
   container concept is minimal: it must be able to expose the key used for
   ordering, swap elements at two indices, and provide and assign aggregated
   records.  These requirements allow the algorithm to reorder multiple parallel
   arrays simultaneously (e.g. keys and values) while using the same high
   quality partitioning and pivot selection strategy as the original pdqsort.

    The code below mirrors the logic of Orson Peters' pdqsort implementation but
   has been carefully adapted to operate on index based containers rather than
   iterators.  The main adaptation points are:

      * Comparisons against the pivot are expressed in terms of key accessors
   rather than using generic callables.  The container must therefore expose a
   `get_key(i)` member returning a const reference to the key at index `i`.  All
   ordering is performed via the `<` operator on keys.

      * Moving and copying of elements during insertion sort and pivot placement
   is done via the container’s `get_record(i)` and `set_record(i, rec)`
   functions.  A record is an aggregated value containing the key and associated
   payloads (for example a tuple of the key and values).  Storing a record at an
   index writes all constituent arrays in one call.

      * Swapping two elements uses the container’s `swap(i, j)` member, which
   must exchange all keys and associated values.

    To use this header you should define a container type satisfying the
   following concept:

        template<typename C>
        concept sortable_container = requires(C c, std::size_t i, std::size_t j,
                                              typename C::record_type rec) {
            { c.size() } -> std::same_as<std::size_t>;
            { c.get_key(i) };
            { c.get_record(i) } -> std::same_as<typename C::record_type>;
            { c.set_record(i, rec) } -> std::same_as<void>;
            { c.swap(i, j) } -> std::same_as<void>;
        };

    An example structure‑of‑arrays container that satisfies this concept might
   look like this:

        template<typename Key, typename... Values>
        struct SoA {
            using key_type   = Key;
            using record_type = std::tuple<Key, Values...>;

            std::vector<Key>     keys;
            std::tuple<std::vector<Values>...> values;

            std::size_t size() const { return keys.size(); }

            const Key& get_key(std::size_t i) const { return keys[i]; }
            record_type get_record(std::size_t i) const {
                return get_record_impl(i, std::index_sequence_for<Values...>{});
            }
            template<std::size_t... Is>
            record_type get_record_impl(std::size_t i,
   std::index_sequence<Is...>) const { return { keys[i],
   std::get<Is>(values)[i]... };
            }
            void set_record(std::size_t i, const record_type& rec) {
                set_record_impl(i, rec, std::index_sequence_for<Values...>{});
            }
            template<std::size_t... Is>
            void set_record_impl(std::size_t i, const record_type& rec,
                                 std::index_sequence<Is...>) {
                keys[i] = std::get<0>(rec);
                ((std::get<Is>(values)[i] = std::get<Is+1>(rec)), ...);
            }
            void swap(std::size_t i, std::size_t j) {
                using std::swap;
                swap(keys[i], keys[j]);
                swap_values_impl<0>(i, j);
            }
        private:
            template<std::size_t Index>
            void swap_values_impl(std::size_t i, std::size_t j) {
                if constexpr (Index < sizeof...(Values)) {
                    using std::swap;
                    swap(std::get<Index>(values)[i],
   std::get<Index>(values)[j]); swap_values_impl<Index + 1>(i, j);
                }
            }
        };

    You can also wrap a conventional array‑of‑structures container with a small
   adapter to satisfy the same concept.  See the tests for examples of how to do
   this.

    After defining a suitable container you can call `pdqsort_soa` with either
   the entire container or a subrange of indices.

        pdqsort_soa(container); // sorts the entire container
        pdqsort_soa(0, n, container); // sorts elements in [0, n)

    This implementation requires a C++20 compiler.

*/

#ifndef PDQSORT_SOA_H
#define PDQSORT_SOA_H

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
template <typename C>
concept SortableContainer = requires(C c, std::size_t i, std::size_t j, typename C::record_type rec) {
  { c.size() } -> std::same_as<std::size_t>;
  { c.get_key(i) };
  { c.get_record(i) } -> std::same_as<typename C::record_type>;
  { c.set_record(i, rec) } -> std::same_as<void>;
  { c.swap(i, j) } -> std::same_as<void>;
};
namespace pdqsort_soa_detail
{

// Constants controlling the behaviour of the algorithm.  These mirror the
// values from Orson Peters' reference implementation.  See the comments in
// pdqsort.h for a detailed description of each constant.
enum
{
  insertion_sort_threshold     = 24,
  ninther_threshold            = 128,
  partial_insertion_sort_limit = 8,
  block_size                   = 64,
  cacheline_size               = 64
};

// Compute floor(log2(n)).  Assumes n > 0.
inline int log2_upto(std::size_t n)
{
  int log = 0;
  while (n >>= 1)
    ++log;
  return log;
}

/**
 * A small helper that returns true if the type T is considered a “simple”
 * arithmetic type. We use this trait to choose between the branchless and
 * non‑branchless partitioning code.
 */
template <typename T>
struct is_simple_arithmetic : std::integral_constant<bool, std::is_arithmetic_v<T> && !std::is_same_v<T, bool>>
{};

/**
 * Determine if the container’s key type is suitable for branchless comparisons.
 * Only basic arithmetic types use the branchless partitioning; more complex key
 * types may exhibit different branching behaviour so we conservatively use the
 * simpler partition in that case.
 */
template <SortableContainer Container>
constexpr bool can_use_branchless()
{
  if constexpr (requires { typename Container::key_type; })
  {
    using K = typename Container::key_type;
    return is_simple_arithmetic<K>::value;
  }
  else
  {
    return false;
  }
}

/**
 * Compare the key at index `i` against the key at index `j`.  Returns true if
 * the key at `i` is strictly less than the key at `j`.
 */
template <SortableContainer Container>
inline bool key_less(Container const& c, std::size_t i, std::size_t j)
{
  return c.get_key(i) < c.get_key(j);
}

/**
 * Compare the container key at index `i` against the key stored in a record.
 * Returns true if c.get_key(i) < std::get<0>(rec).
 */
template <SortableContainer Container>
inline bool idx_less_record(Container const& c, std::size_t i, typename Container::record_type const& rec)
{
  return c.get_key(i) < std::get<0>(rec);
}

/**
 * Compare the key stored in a record against the container key at index `j`.
 * Returns true if std::get<0>(rec) < c.get_key(j).
 */
template <SortableContainer Container>
inline bool record_less_idx(Container const& c, typename Container::record_type const& rec, std::size_t j)
{
  return std::get<0>(rec) < c.get_key(j);
}

/**
 * Perform insertion sort on the range [first, last).  Sorting is done in
 * ascending order according to the container’s keys.  This function is used for
 * small partitions where insertion sort outperforms quicksort.
 */
template <SortableContainer Container>
void insertion_sort(std::size_t first, std::size_t last, Container& c)
{
  if (first >= last)
    return;
  for (std::size_t cur = first + 1; cur < last; ++cur)
  {
    std::size_t sift   = cur;
    std::size_t sift_1 = cur - 1;
    // Avoid two moves when the element is already positioned correctly.
    if (key_less(c, sift, sift_1))
    {
      // Save the current record.
      auto tmp = c.get_record(sift);
      do
      {
        c.set_record(sift, c.get_record(sift_1));
        --sift;
      }
      while (sift > first && record_less_idx(c, tmp, --sift_1));
      c.set_record(sift, std::move(tmp));
    }
  }
}

/**
 * Perform insertion sort on [first, last) under the assumption that the element
 * at index `first-1` is less than or equal to all elements in [first, last).
 * This allows the comparisons in the inner loop to drop a range check and is
 * slightly faster in practice.
 */
template <SortableContainer Container>
void unguarded_insertion_sort(std::size_t first, std::size_t last, Container& c)
{
  if (first >= last)
    return;
  for (std::size_t cur = first + 1; cur < last; ++cur)
  {
    std::size_t sift   = cur;
    std::size_t sift_1 = cur - 1;
    if (key_less(c, sift, sift_1))
    {
      auto tmp = c.get_record(sift);
      do
      {
        c.set_record(sift, c.get_record(sift_1));
        --sift;
      }
      while (record_less_idx(c, tmp, --sift_1));
      c.set_record(sift, std::move(tmp));
    }
  }
}

/**
 * Attempt an insertion sort on [first, last).  If more than
 * `partial_insertion_sort_limit` elements are moved the function gives up and
 * returns false. Otherwise it completes the insertion sort and returns true.
 * This is used when the partition is already nearly sorted.
 */
template <SortableContainer Container>
bool partial_insertion_sort(std::size_t first, std::size_t last, Container& c)
{
  if (first >= last)
    return true;
  std::size_t moves = 0;
  for (std::size_t cur = first + 1; cur < last; ++cur)
  {
    std::size_t sift   = cur;
    std::size_t sift_1 = cur - 1;
    if (key_less(c, sift, sift_1))
    {
      auto tmp = c.get_record(sift);
      do
      {
        c.set_record(sift, c.get_record(sift_1));
        --sift;
      }
      while (sift > first && record_less_idx(c, tmp, --sift_1));
      c.set_record(sift, std::move(tmp));
      moves += cur - sift;
      if (moves > partial_insertion_sort_limit)
        return false;
    }
  }
  return true;
}

/**
 * Swap two elements if they are out of order.
 */
template <SortableContainer Container>
inline void sort2(std::size_t a, std::size_t b, Container& c)
{
  if (key_less(c, b, a))
    c.swap(a, b);
}

/**
 * Sort three elements with indices a, b and c in ascending order using only
 * comparisons and swaps.  This is used when selecting the pivot.
 */
template <SortableContainer Container>
inline void sort3(std::size_t a, std::size_t b, std::size_t c, Container& cont)
{
  sort2(a, b, cont);
  sort2(b, c, cont);
  sort2(a, b, cont);
}

/**
 * Align a pointer to the next cache line.  Used by branchless partitioning to
 * store temporary offset arrays without causing false sharing.
 */
template <typename T>
inline T* align_cacheline(T* p)
{
  std::uintptr_t ip = reinterpret_cast<std::uintptr_t>(p);
  ip                = (ip + cacheline_size - 1) & static_cast<std::uintptr_t>(-cacheline_size);
  return reinterpret_cast<T*>(ip);
}

/**
 * Swap a batch of pairs of elements determined by the offset arrays.  When
 * `use_swaps` is false the function performs a batched rotation which saves a
 * temporary move; when `use_swaps` is true it does elementwise swaps.  This
 * helper is used by the branchless partitioning code.
 */
template <SortableContainer Container>
inline void swap_offsets(std::size_t    base_l,
                         std::size_t    base_r,
                         unsigned char* offsets_l,
                         unsigned char* offsets_r,
                         std::size_t    num,
                         bool           use_swaps,
                         Container&     c)
{
  if (use_swaps)
  {
    // When the distribution is descending we need to use proper swaps to remain
    // O(n).
    for (std::size_t i = 0; i < num; ++i)
    {
      c.swap(base_l + offsets_l[i], base_r - offsets_r[i]);
    }
  }
  else if (num > 0)
  {
    // Perform a cyclic rotation amongst all mismatched elements.
    std::size_t l   = base_l + offsets_l[0];
    std::size_t r   = base_r - offsets_r[0];
    auto        tmp = c.get_record(l);
    c.set_record(l, c.get_record(r));
    for (std::size_t i = 1; i < num; ++i)
    {
      l = base_l + offsets_l[i];
      c.set_record(r, c.get_record(l));
      r = base_r - offsets_r[i];
      c.set_record(l, c.get_record(r));
    }
    c.set_record(r, std::move(tmp));
  }
}

/**
 * Partition the range [first, last) around the pivot stored at index `first`.
 * Elements strictly less than the pivot are moved to the left and elements
 * greater than or equal to the pivot are moved to the right.  The function
 * returns the final position of the pivot and a flag indicating whether the
 * input sequence was already partitioned.  This is the simple (non‑branchless)
 * partition used when the key type is complex or when branchless is disabled.
 */
template <SortableContainer Container>
std::pair<std::size_t, bool> partition_right(std::size_t first, std::size_t last, Container& c)
{
  // Copy pivot out of the array. We'll place it back once partitioning is
  // complete.
  auto        pivot = c.get_record(first);
  std::size_t f     = first;
  std::size_t l     = last;

  // Find first element >= pivot on the left side.
  while (idx_less_record(c, ++f, pivot))
  {
    ;
  }
  // Find first element < pivot on the right side, with guard if needed.
  if (f - 1 == first)
  {
    while (f < l && !idx_less_record(c, --l, pivot))
    {
      ;
    }
  }
  else
  {
    while (!idx_less_record(c, --l, pivot))
    {
      ;
    }
  }

  bool already_partitioned = f >= l;
  // Keep swapping pairs until the pointers cross.
  while (f < l)
  {
    c.swap(f, l);
    while (idx_less_record(c, ++f, pivot))
    {
      ;
    }
    while (!idx_less_record(c, --l, pivot))
    {
      ;
    }
  }

  // Place pivot right before f
  std::size_t pivot_pos = f - 1;
  c.set_record(first, c.get_record(pivot_pos));
  c.set_record(pivot_pos, std::move(pivot));
  return {pivot_pos, already_partitioned};
}

/**
 * Partition [first, last) placing elements equal to the pivot into the left
 * partition. This variant is used only when the data contains many duplicate
 * keys.  Elements strictly greater than the pivot are moved to the right;
 * elements less than or equal to the pivot stay on the left.  The pivot is the
 * element originally at `first` and is placed back into its correct position at
 * the end.  The function returns the final index of the pivot.
 */
template <SortableContainer Container>
std::size_t partition_left(std::size_t first, std::size_t last, Container& c)
{
  auto        pivot = c.get_record(first);
  std::size_t f     = first;
  std::size_t l     = last;
  // Move l leftwards while pivot < c[l-1] (i.e., while the element at l-1 is
  // strictly greater than the pivot).  We want to stop on an element <= pivot.
  while (l > f && record_less_idx(c, pivot, --l))
  {
    ;
  }
  // Move f rightwards while c[f+1] <= pivot.  We stop on an element > pivot.
  if (l + 1 == last)
  {
    while (f < l && !record_less_idx(c, pivot, ++f))
    {
      ;
    }
  }
  else
  {
    while (!record_less_idx(c, pivot, ++f))
    {
      ;
    }
  }
  // Swap out‑of‑place elements until the pointers cross.
  while (f < l)
  {
    c.swap(f, l);
    // Move l again leftwards while c[l-1] > pivot.
    while (record_less_idx(c, pivot, --l))
    {
      ;
    }
    // Move f rightwards while c[f] <= pivot.
    while (!record_less_idx(c, pivot, ++f))
    {
      ;
    }
  }
  std::size_t pivot_pos = l;
  c.set_record(first, c.get_record(pivot_pos));
  c.set_record(pivot_pos, std::move(pivot));
  return pivot_pos;
}

/**
 * Branchless partitioning routine.  For containers where the key type is a
 * simple arithmetic type this overload would normally employ a branchless block
 * partitioning strategy to reduce branch mispredictions on random data.
 * Implementing that strategy generically for arbitrary containers is
 * error‑prone and fragile.  Instead we simply forward to the regular partition
 * function.  While this loses some micro‑optimisation on uniformly random
 * inputs, it still exhibits excellent performance and keeps the implementation
 * straightforward.  The return value is identical to partition_right.
 */
template <SortableContainer Container>
std::pair<std::size_t, bool> partition_right_branchless(std::size_t first, std::size_t last, Container& c)
{
  // Branchless block partitioning adapted from pdqsort for index-based
  // container
  auto        pivot     = c.get_record(first);
  auto const& pivot_key = std::get<0>(pivot);
  std::size_t f         = first;
  std::size_t l         = last;

  // Find first element >= pivot
  while (c.get_key(++f) < pivot_key)
  {
    ;
  }
  // Find first element < pivot with guard
  if (f - 1 == first)
  {
    while (f < l && !(c.get_key(--l) < pivot_key))
    {
      ;
    }
  }
  else
  {
    while (!(c.get_key(--l) < pivot_key))
    {
      ;
    }
  }

  bool already_partitioned = f >= l;
  if (!already_partitioned)
  {
    c.swap(f, l);
    ++f;

    unsigned char  offsets_l_storage[block_size + cacheline_size];
    unsigned char  offsets_r_storage[block_size + cacheline_size];
    unsigned char* offsets_l = align_cacheline(offsets_l_storage);
    unsigned char* offsets_r = align_cacheline(offsets_r_storage);

    std::size_t base_l = f;
    std::size_t base_r = l;
    std::size_t num_l = 0, num_r = 0, start_l = 0, start_r = 0;

    while (f < l)
    {
      // Determine how many elements to examine for each side
      std::size_t num_unknown = l - f;
      std::size_t left_split  = num_l == 0 ? (num_r == 0 ? num_unknown / 2 : num_unknown) : 0;
      std::size_t right_split = num_r == 0 ? (num_unknown - left_split) : 0;

      // Fill left offsets: store indices i in [0, left_split) where key >=
      // pivot
      if (left_split >= block_size)
      {
        for (std::size_t i = 0; i < block_size;)
        {
          offsets_l[num_l] = i++;
          num_l += !(c.get_key(f) < pivot_key);
          ++f;
          offsets_l[num_l] = i++;
          num_l += !(c.get_key(f) < pivot_key);
          ++f;
          offsets_l[num_l] = i++;
          num_l += !(c.get_key(f) < pivot_key);
          ++f;
          offsets_l[num_l] = i++;
          num_l += !(c.get_key(f) < pivot_key);
          ++f;
          offsets_l[num_l] = i++;
          num_l += !(c.get_key(f) < pivot_key);
          ++f;
          offsets_l[num_l] = i++;
          num_l += !(c.get_key(f) < pivot_key);
          ++f;
          offsets_l[num_l] = i++;
          num_l += !(c.get_key(f) < pivot_key);
          ++f;
          offsets_l[num_l] = i++;
          num_l += !(c.get_key(f) < pivot_key);
          ++f;
        }
      }
      else
      {
        for (std::size_t i = 0; i < left_split;)
        {
          offsets_l[num_l] = i++;
          num_l += !(c.get_key(f) < pivot_key);
          ++f;
        }
      }

      // Fill right offsets: store distances i in (0, right_split] where key <
      // pivot
      if (right_split >= block_size)
      {
        for (std::size_t i = 0; i < block_size;)
        {
          offsets_r[num_r] = ++i;
          num_r += (c.get_key(--l) < pivot_key);
          offsets_r[num_r] = ++i;
          num_r += (c.get_key(--l) < pivot_key);
          offsets_r[num_r] = ++i;
          num_r += (c.get_key(--l) < pivot_key);
          offsets_r[num_r] = ++i;
          num_r += (c.get_key(--l) < pivot_key);
          offsets_r[num_r] = ++i;
          num_r += (c.get_key(--l) < pivot_key);
          offsets_r[num_r] = ++i;
          num_r += (c.get_key(--l) < pivot_key);
          offsets_r[num_r] = ++i;
          num_r += (c.get_key(--l) < pivot_key);
          offsets_r[num_r] = ++i;
          num_r += (c.get_key(--l) < pivot_key);
        }
      }
      else
      {
        for (std::size_t i = 0; i < right_split;)
        {
          offsets_r[num_r] = ++i;
          num_r += (c.get_key(--l) < pivot_key);
        }
      }

      // Swap elements and update state
      std::size_t num = std::min(num_l, num_r);
      swap_offsets(base_l, base_r, offsets_l + start_l, offsets_r + start_r, num, num_l == num_r, c);
      num_l -= num;
      num_r -= num;
      start_l += num;
      start_r += num;

      if (num_l == 0)
      {
        start_l = 0;
        base_l  = f;
      }
      if (num_r == 0)
      {
        start_r = 0;
        base_r  = l;
      }
    }

    // Final swaps for any remaining offsets
    if (num_l)
    {
      offsets_l += start_l;
      while (num_l--)
      {
        c.swap(base_l + offsets_l[num_l], --l);
      }
      f = l;
    }
    if (num_r)
    {
      offsets_r += start_r;
      while (num_r--)
      {
        c.swap(base_r - offsets_r[num_r], f);
        ++f;
      }
      l = f;
    }
  }

  // Place pivot
  std::size_t pivot_pos = f - 1;
  c.set_record(first, c.get_record(pivot_pos));
  c.set_record(pivot_pos, std::move(pivot));
  return {pivot_pos, already_partitioned};
}

/**
 * The core quicksort loop.  This function recurses on smaller partitions and
 * performs insertion sort for tiny partitions.  It also monitors recursion
 * depth and falls back to heapsort when the recursion depth becomes too large.
 * The `Branchless` template parameter selects whether to use the branchless or
 * regular partitioning code.
 */
template <typename Container, bool Branchless>
void pdqsort_loop(std::size_t first, std::size_t last, Container& c, int bad_allowed, bool leftmost)
{
  using diff_t = std::ptrdiff_t;
  while (true)
  {
    diff_t size = static_cast<diff_t>(last - first);
    // Insertion sort on small ranges.
    if (size < static_cast<diff_t>(insertion_sort_threshold))
    {
      if (leftmost)
        insertion_sort(first, last, c);
      else
        unguarded_insertion_sort(first, last, c);
      return;
    }
    // Choose a pivot via median of 3 or pseudomedian of 9.
    diff_t s2 = size / 2;
    if (size > static_cast<diff_t>(ninther_threshold))
    {
      sort3(first, first + s2, last - 1, c);
      sort3(first + 1, first + (s2 - 1), last - 2, c);
      sort3(first + 2, first + (s2 + 1), last - 3, c);
      sort3(first + (s2 - 1), first + s2, first + (s2 + 1), c);
      c.swap(first, first + s2);
    }
    else
    {
      sort3(first + s2, first, last - 1, c);
    }
    // If the element before `first` is the sentinel from a previous partition
    // and it is equal to the current pivot then we partition equal elements to
    // the left.  We skip the left partition as it is already sorted.
    if (!leftmost)
    {
      // There is a guarantee that `first > 0` when !leftmost.  We cannot access
      // c.get_key(first - 1) if first == 0.  However the caller always passes
      // leftmost = true for the initial call and sets it to false only after
      // recursing into the right half.  Therefore first > 0 here.
      if (!(key_less(c, first - 1, first)))
      {
        first = partition_left(first, last, c) + 1;
        continue;
      }
    }
    // Partition the range around the chosen pivot.  We select the
    // implementation based on the Branchless template parameter.  Branchless
    // partitioning is used for simple arithmetic key types and improves
    // performance on random data distributions.
    std::pair<std::size_t, bool> part_result =
      Branchless ? partition_right_branchless(first, last, c) : partition_right(first, last, c);
    std::size_t pivot_pos           = part_result.first;
    bool        already_partitioned = part_result.second;
    diff_t      left_size           = static_cast<diff_t>(pivot_pos) - static_cast<diff_t>(first);
    diff_t      right_size          = static_cast<diff_t>(last) - static_cast<diff_t>(pivot_pos + 1);
    bool        highly_unbalanced   = left_size < size / 8 || right_size < size / 8;
    if (highly_unbalanced)
    {
      // Too many bad partitions? fall back to heapsort.
      if (--bad_allowed == 0)
      {
        // Build a heap and sort it.  We implement a simple heap here using the
        // container interface.  Because the container exposes only high level
        // operations we implement a minimal binary heap sort on indices [first,
        // last).
        auto heapify = [&](std::size_t count)
        {
          // Start from the first non leaf node and sift down.
          if (count < 2)
            return;
          for (std::size_t idx = (count / 2); idx > 0; --idx)
          {
            std::size_t i = idx - 1;
            while (true)
            {
              std::size_t left    = 2 * i + 1;
              std::size_t right   = left + 1;
              std::size_t largest = i;
              if (left < count && key_less(c, first + largest, first + left))
              {
                largest = left;
              }
              if (right < count && key_less(c, first + largest, first + right))
              {
                largest = right;
              }
              if (largest != i)
              {
                c.swap(first + i, first + largest);
                i = largest;
              }
              else
                break;
            }
          }
        };
        auto pop_heap = [&](std::size_t count)
        {
          // Swap root with the last element and sift down.
          c.swap(first, first + (count - 1));
          std::size_t i = 0;
          while (true)
          {
            std::size_t left    = 2 * i + 1;
            std::size_t right   = left + 1;
            std::size_t largest = i;
            if (left < count - 1 && key_less(c, first + largest, first + left))
            {
              largest = left;
            }
            if (right < count - 1 && key_less(c, first + largest, first + right))
            {
              largest = right;
            }
            if (largest != i)
            {
              c.swap(first + i, first + largest);
              i = largest;
            }
            else
              break;
          }
        };
        std::size_t count = last - first;
        heapify(count);
        while (count > 1)
        {
          pop_heap(count);
          --count;
        }
        return;
      }
      // Shuffle some elements into both partitions to break monotonic patterns.
      // This step helps maintain the O(n log n) average complexity even on
      // nearly sorted input.  We perform a few swaps based on a fraction of
      // each partition size.
      if (left_size >= static_cast<diff_t>(insertion_sort_threshold))
      {
        c.swap(first, first + left_size / 4);
        c.swap(pivot_pos - 1, pivot_pos - left_size / 4);
        if (left_size > static_cast<diff_t>(ninther_threshold))
        {
          c.swap(first + 1, first + (left_size / 4 + 1));
          c.swap(first + 2, first + (left_size / 4 + 2));
          c.swap(pivot_pos - 2, pivot_pos - (left_size / 4 + 1));
          c.swap(pivot_pos - 3, pivot_pos - (left_size / 4 + 2));
        }
      }
      if (right_size >= static_cast<diff_t>(insertion_sort_threshold))
      {
        c.swap(pivot_pos + 1, pivot_pos + 1 + right_size / 4);
        c.swap(last - 1, last - right_size / 4);
        if (right_size > static_cast<diff_t>(ninther_threshold))
        {
          c.swap(pivot_pos + 2, pivot_pos + 2 + right_size / 4);
          c.swap(pivot_pos + 3, pivot_pos + 3 + right_size / 4);
          c.swap(last - 2, last - (1 + right_size / 4));
          c.swap(last - 3, last - (2 + right_size / 4));
        }
      }
    }
    else
    {
      // If perfectly balanced and the data was already partitioned try
      // insertion sort.
      if (already_partitioned && partial_insertion_sort(first, pivot_pos, c) &&
          partial_insertion_sort(pivot_pos + 1, last, c))
      {
        return;
      }
    }
    // Recurse on the smaller partition first to reduce stack depth.  Then loop
    // on the larger partition (tail recursion elimination).
    pdqsort_loop<Container, Branchless>(first, pivot_pos, c, bad_allowed, leftmost);
    first    = pivot_pos + 1;
    leftmost = false;
  }
}

} // namespace pdqsort_soa_detail

/**
 * Sort the elements of `container` in ascending order using pattern‑defeating
 * quicksort.  This overload sorts the entire container range [0,
 * container.size()).  The container must satisfy the concept described at the
 * top of the file.
 */
template <SortableContainer Container>
void pdqsort_soa(Container& container)
{
  if (container.size() <= 1)
    return;
  using pdqsort_soa_detail::can_use_branchless;
  constexpr bool branchless  = pdqsort_soa_detail::can_use_branchless<Container>();
  int            bad_allowed = pdqsort_soa_detail::log2_upto(container.size());
  if constexpr (branchless)
  {
    pdqsort_soa_detail::pdqsort_loop<Container, true>(0, container.size(), container, bad_allowed, true);
  }
  else
  {
    pdqsort_soa_detail::pdqsort_loop<Container, false>(0, container.size(), container, bad_allowed, true);
  }
}

/**
 * Sort the subrange [first, last) of `container` in ascending order using
 * pattern‑defeating quicksort.  Indices are zero based and `last` is exclusive.
 * If `first >= last` nothing is done.  The container must satisfy the concept
 * described at the top of the file.
 */
template <SortableContainer Container>
void pdqsort_soa(std::size_t first, std::size_t last, Container& container)
{
  if (first >= last || last > container.size())
    return;
  using pdqsort_soa_detail::can_use_branchless;
  constexpr bool branchless  = pdqsort_soa_detail::can_use_branchless<Container>();
  int            bad_allowed = pdqsort_soa_detail::log2_upto(last - first);
  if constexpr (branchless)
  {
    pdqsort_soa_detail::pdqsort_loop<Container, true>(first, last, container, bad_allowed, true);
  }
  else
  {
    pdqsort_soa_detail::pdqsort_loop<Container, false>(first, last, container, bad_allowed, true);
  }
}

#endif // PDQSORT_SOA_H