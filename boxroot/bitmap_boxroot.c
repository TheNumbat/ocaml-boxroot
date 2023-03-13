/* SPDX-License-Identifier: MIT */
/* {{{ Includes */

// This is emacs folding-mode

#include <assert.h>
#include <errno.h>
#include <limits.h>
#if defined(ENABLE_BOXROOT_MUTEX) && (ENABLE_BOXROOT_MUTEX == 1)
#include <pthread.h>
#endif
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CAML_NAME_SPACE
#define CAML_INTERNALS

#include "bitmap_boxroot.h"
#include <caml/minor_gc.h>
#include <caml/major_gc.h>

#if defined(_POSIX_TIMERS) && defined(_POSIX_MONOTONIC_CLOCK)
#define POSIX_CLOCK
#include <time.h>
#endif

#include "ocaml_hooks.h"
#include "platform.h"

/* }}} */

/* {{{ Config */

#define GENERATIONAL 1
#define THREAD_SAFE 1

/* }}} */

/* {{{ Data types */

typedef unsigned long bitmap;
#define CHUNK_SIZE ((int)(8 * sizeof(bitmap)))

#ifdef __GNUC__
inline unsigned count_trailing_zeros(bitmap x) {
  DEBUGassert(x != 0);
  return __builtin_ctzl(x);
}
#else
static_assert(false, "No count_trailing_zeros defined on this platform");
#endif

// values are stored in "ring elements"
// rings: cyclic doubly-linked lists

typedef struct chunk {
  value slot[CHUNK_SIZE];
  struct chunk *prev;
  struct chunk *next;
  bool is_young;
  _Atomic(bitmap) free;
} chunk;

/* Chunks are aligned */
#define CHUNK_ALIGNMENT ((uintptr_t)(CHUNK_SIZE * sizeof(value)))
#define CHUNK_MASK (CHUNK_ALIGNMENT - 1)

#define BITMAP_EMPTY (~(bitmap)0)

typedef chunk *ring;

static inline value *chunk_index(chunk *chunk, int index)
{
  DEBUGassert(0 <= index && index < CHUNK_SIZE);
  return &chunk->slot[index];
}

/* }}} */

/* {{{ Globals */

/* Global rings. */
static struct {
  /* list of chunks with young values. All non-full chunks at the start. */
  ring young;

  /* list of chunks with old values. All non-full chunks at the start. */
  ring old;
} rings;

mutex_t rings_mutex = PTHREAD_MUTEX_INITIALIZER;

#if THREAD_SAFE

static void lock_rings(void) { bxr_mutex_lock(&rings_mutex); }
static void unlock_rings(void) { bxr_mutex_unlock(&rings_mutex); }
static bitmap atomic_xor(_Atomic(bitmap) *dst, bitmap src)
{
  return atomic_fetch_xor_explicit(dst, src, memory_order_relaxed);
}

#else

static void lock_rings(void) { }
static void unlock_rings(void) { }
static bitmap atomic_xor(_Atomic(bitmap) *dst, bitmap src)
{
  bitmap old = load_relaxed(dst);
  store_relaxed(dst, old ^ src);
  return old;
}

#endif

static void validate_all_rings();

/* Not made thread-safe yet */
struct stats {
  int minor_collections;
  int major_collections;
  int total_create;
  int total_delete;
  int total_modify;
  long long total_scanning_work_minor;
  long long total_scanning_work_major;
  int64_t total_minor_time;
  int64_t total_major_time;
  int64_t peak_minor_time;
  int64_t peak_major_time;
  long long total_alloced_pools;
  long long total_emptied_pools;
  long long total_freed_pools;
  long long peak_pools; // max live pools at any time
  long long is_young; // count 'is_young_block' checks
  long long ring_operations; // Number of times p->next is mutated
};

static struct stats stats;

/* }}} */

/* {{{ Ring operations */

/* Iterate on all elements of a ring.
   [elem]: a variable of type [struct elem *] -- bound by the macro.
   [ring]: a variable of type [ring] -- the ring we iterate over.
   [action]: an expression that can refer to [elem],
             and should preserve the validity of [elem] and [ring].
*/
#define FOREACH_ELEM_IN_RING(elem, r, action) do {             \
    ring __end = (r);                                          \
    if (__end == NULL) break;                                  \
    chunk *__chunk = __end;                                    \
    do {                                                       \
      FOREACH_ELEM_IN_CHUNK(elem, __chunk, action);            \
      __chunk = __chunk->next;                                 \
    } while (__chunk != __end);                                \
  } while (0)

#define FOREACH_ELEM_IN_CHUNK(elem, chunk, action) do {                 \
    bitmap allocated = ~load_acquire(&chunk->free);                     \
    bitmap bitmask = 1;                                                 \
    for (int i = 0; i < CHUNK_SIZE; i++) {                              \
      if (allocated & bitmask) {                                        \
        value *elem = chunk_index(chunk, i);                            \
        action;                                                         \
      }                                                                 \
      bitmask <<= 1;                                                    \
    };                                                                  \
  } while (0)


inline static void ring_link(ring p, ring q)
{
  p->next = q;
  q->prev = p;
  if (DEBUG) incr(&stats.ring_operations);
}

// insert the ring [source] at the back of [*target].
static void ring_push_back(ring source, ring *target)
{
  DEBUGassert(source != NULL);
  if (*target == NULL) {
    *target = source;
  } else {
    chunk *target_last = (*target)->prev;
    chunk *source_last = source->prev;
    ring_link(target_last, source);
    ring_link(source_last, *target);
  }
}

// insert the ring [source] at the front of [*target].
static void ring_push_front(ring source, ring *target)
{
  ring_push_back(source, target);
  *target = source;
}

// remove the first element from [*target] and return it
static ring ring_pop(ring *target)
{
  chunk *front = *target;
  DEBUGassert(front);
  if (front->next == front) {
    *target = NULL;
  } else {
    *target = front->next;
    ring_link(front->prev, front->next);
  }
  ring_link(front, front);
  return front;
}

static void ring_remove_chunk(ring chunk)
{
  ring prev = ring_pop(&chunk);
  if (rings.old == prev) rings.old = chunk;
  if (rings.young == prev) rings.young = chunk;
}

static void free_ring(ring r) {
  if (r == NULL) return;
  chunk *cur = r;
  do {
    chunk *next = cur->next;
    free(cur);
    cur = next;
  } while (cur != r);
}
/* }}} */

/* {{{ Ring of free elements */

#define assert_pow2(x) static_assert(!((x) & ((x)-1)), "not a pow2")

assert_pow2(CHUNK_ALIGNMENT);

static chunk *chunk_of_root(void *root)
{
  return (chunk *)((uintptr_t)root & ~CHUNK_MASK);
}

static ring create_chunk()
{
  chunk *new = NULL;
  if (0 != posix_memalign((void **)&new, CHUNK_ALIGNMENT, sizeof(chunk))) {
    DEBUGassert(false);
    return NULL;
  }
  long long live_pools = ++stats.total_alloced_pools - stats.total_freed_pools;
  if (live_pools > stats.peak_pools) stats.peak_pools = live_pools;
  ring_link(new, new);
  for (int i = 0; i < CHUNK_SIZE; i++) *chunk_index(new, i) = 0;
  store_relaxed(&new->free, BITMAP_EMPTY);
  new->is_young = false;
  DEBUGassert(new == chunk_of_root(&new->slot[0]));
  DEBUGassert(new == chunk_of_root(&new->slot[CHUNK_SIZE - 1]));
  return new;
}

static void delete_chunk(chunk *chunk)
{
  free(chunk);
  ++stats.total_freed_pools;
}

static bool chunk_is_full(chunk *chunk) {
  return !load_relaxed(&chunk->free);
}

/* holds the lock */
static chunk *get_available_chunk(bool young)
{
  ring *target = young ? &rings.young : &rings.old;
  chunk *new = *target;
  if (new != NULL && !chunk_is_full(new)) return new;
  if (young && rings.old != NULL && !chunk_is_full(rings.old)) {
    /* demote an old chunk */
    new = ring_pop(&rings.old);
  } else {
    /* push a new empty chunk */
    new = create_chunk();
  }
  new->is_young = young;
  ring_push_front(new, target);
  DEBUGassert(!chunk_is_full(new));
  return new;
}

/* holds the lock */
static void reclassify_chunk(chunk *chunk)
{
  bitmap free = load_relaxed(&chunk->free);
  ring *target = chunk->is_young ? &rings.young : &rings.old;
  ring_remove_chunk(chunk);
  if (free == BITMAP_EMPTY) { delete_chunk(chunk); }
  else if (free == 0) { ring_push_back(chunk, target); }
  else { ring_push_front(chunk, target); }
}

/* holds the lock */
static void alloc_from_chunk(chunk *chunk, value init, value **root_out)
{
  bitmap free = load_relaxed(&chunk->free);
  DEBUGassert(free != 0);
  int index = count_trailing_zeros(free);
  value *slot = chunk_index(chunk, index);
  *slot = init;
  *root_out = slot;
  bitmap bitmask = (bitmap)1 << index;
  bitmap old = atomic_xor(&chunk->free, bitmask);
  DEBUGassert((chunk->free & bitmask) == 0);
//  DEBUGassert((chunk->free | bitmask) == old); // RACY
  bool is_full = !(old ^ bitmask);
//  DEBUGassert(is_full == chunk_is_full(chunk)); // RACY
  if (is_full) reclassify_chunk(chunk);
}

/* Returns whether the chunk is a candidate for reclassifying */
static bool remove_from_chunk(value *slot, chunk *chunk)
{
  int index = slot - chunk->slot;
  DEBUGassert(index >= 0 && index < CHUNK_SIZE);
  bitmap free = load_relaxed(&chunk->free);
  bitmap bitmask = (bitmap)1 << index;
  DEBUGassert((free & bitmask) == 0);
  bitmap old = atomic_xor(&chunk->free, bitmask);
  DEBUGassert(chunk->free & bitmask);
  bool was_full = !old;
  bool is_empty = ((old ^ bitmask) == BITMAP_EMPTY);
  DEBUGassert((chunk->free == BITMAP_EMPTY) == is_empty); // RACY
  return was_full || is_empty;
}

/* }}} */

/* {{{ Boxroot API implementation */

static inline int is_young_block(value v)
{
  if (DEBUG) ++stats.is_young;
  return Is_block(v) && Is_young(v);
}

bitmap_boxroot bitmap_boxroot_create(value init)
{
  if (DEBUG) ++stats.total_create;
  bool young = GENERATIONAL /* && is_young_block(init) */;
  lock_rings();
  chunk *chunk = get_available_chunk(young);
  value *root;
  alloc_from_chunk(chunk, init, &root);
  unlock_rings();
  return (bitmap_boxroot)root;
}

void bitmap_boxroot_delete(bitmap_boxroot root)
{
  chunk *chunk = chunk_of_root(root);
  bool maybe_reclassify = remove_from_chunk((value *)root, chunk);
  if (maybe_reclassify) {
    lock_rings();
    /* Heuristic: keep empty chunk if at the head */
    if (chunk != rings.young && chunk != rings.old) reclassify_chunk(chunk);
    unlock_rings();
  }
}

void bitmap_boxroot_modify(bitmap_boxroot *root, value new_value)
{
  value *old_value = (value *)*root;
  if (!is_young_block(new_value) || is_young_block(*old_value)) {
    *old_value = new_value;
  } else {
    bitmap_boxroot_delete(*root);
    *root = bitmap_boxroot_create(new_value);
  }
}

/* }}} */

/* {{{ Scanning */

static void validate_all_rings()
{
  lock_rings();
  struct stats stats_before = stats;
  FOREACH_ELEM_IN_RING(elem, rings.young, {
    assert(is_young_block(*elem));
  });
  FOREACH_ELEM_IN_RING(elem, rings.old, {
    assert(!is_young_block(*elem));
  });
  stats = stats_before;
  unlock_rings();
}

// returns the amount of work done
static int scan_ring_gen(scanning_action action, void *data, ring r)
{
  int work = 0;
  FOREACH_ELEM_IN_RING(elem, r, {
      value v = *elem;
      DEBUGassert(v != 0);
      CALL_GC_ACTION(action, data, v, elem);
      work++;
    });
  return work;
}

// returns the amount of work done
static int scan_ring_young(scanning_action action, void *data)
{
#if OCAML_MULTICORE
  /* If a <= b - 2 then
     a < x && x < b  <=>  x - a - 1 <= x - b - 2 (unsigned comparison)
  */
  uintnat young_start = (uintnat)caml_minor_heaps_start + 1;
  uintnat young_range = (uintnat)caml_minor_heaps_end - 1 - young_start;
#else
  uintnat young_start = (uintnat)Caml_state->young_start;
  uintnat young_range = (uintnat)Caml_state->young_end - young_start;
#endif
  int work = 0;
  FOREACH_ELEM_IN_RING(elem, rings.young, {
      value v = *elem;
      DEBUGassert(v != 0);
      if ((uintnat)v - young_start <= young_range
          && BXR_LIKELY(Is_block(v))) {
        CALL_GC_ACTION(action, data, v, elem);
        work++;
      }
    });
  return work;
}

static void scan_roots(scanning_action action, void *data)
{
  int work = 0;
  if (DEBUG) validate_all_rings();
  lock_rings();
  if (GENERATIONAL && bxr_in_minor_collection()) {
    work += scan_ring_young(action, data);
    if (rings.young != NULL) {
      chunk *chunk = rings.young;
      do {
        chunk->is_young = false;
        chunk = chunk->next;
      } while (chunk != rings.young);
      ring_push_back(rings.young, &rings.old);
    }
    rings.young = NULL;
    stats.total_scanning_work_minor += work;
  } else {
    work += scan_ring_gen(action, data, rings.young);
    work += scan_ring_gen(action, data, rings.old);
    stats.total_scanning_work_major += work;
  }
  unlock_rings();
  if (DEBUG) validate_all_rings();
}

/* }}} */

/* {{{ Statistics */

static int64_t time_counter(void)
{
#if defined(POSIX_CLOCK)
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * (int64_t)1000000000 + (int64_t)t.tv_nsec;
#else
  return 0;
#endif
}

// unit: 1=KiB, 2=MiB
static long long kib_of_pools(long long count, int unit)
{
  long long pool_size_b = sizeof(chunk);
  long long unit_size = (long long)1 << (unit * 10);
  double size = (double)pool_size_b / (double)unit_size;
  return (long long)((double)count * size);
}
static double average(long long total, long long units)
{
  // round to nearest
  return ((double)total) / (double)units;
}

void bitmap_boxroot_print_stats()
{
  printf("minor collections: %d\n"
         "major collections (and others): %d\n",
         stats.minor_collections,
         stats.major_collections);

  printf("total allocated pools: %'lld (%'lld MiB)\n"
         "peak allocated pools: %'lld (%'lld MiB)\n"
         "total emptied pools: %'lld (%'lld MiB)\n"
         "total freed pools: %'lld (%'lld MiB)\n",
         stats.total_alloced_pools,
         kib_of_pools(stats.total_alloced_pools, 2),
         stats.peak_pools,
         kib_of_pools(stats.peak_pools, 2),
         stats.total_emptied_pools,
         kib_of_pools(stats.total_emptied_pools, 2),
         stats.total_freed_pools,
         kib_of_pools(stats.total_freed_pools, 2));

#if DEBUG != 0
  printf("total created: %'d\n"
         "total deleted: %'d\n"
         "total modified: %'d\n",
         stats.total_create,
         stats.total_delete,
         stats.total_modify);

  printf("is_young_block: %'lld\n",
         stats.is_young);
#endif

  double ring_operations_per_pool =
    average(stats.ring_operations, stats.total_alloced_pools);

  printf("total ring operations: %'lld\n"
         "ring operations per pool: %.2f\n",
         stats.ring_operations,
         ring_operations_per_pool);

  double scanning_work_minor = average(stats.total_scanning_work_minor, stats.minor_collections);
  double scanning_work_major = average(stats.total_scanning_work_major, stats.major_collections);
  long long total_scanning_work = stats.total_scanning_work_minor + stats.total_scanning_work_major;

  printf("work per minor: %'.0f\n"
         "work per major: %'.0f\n"
         "total scanning work: %'lld (%'lld minor, %'lld major)\n",
         scanning_work_minor,
         scanning_work_major,
         total_scanning_work, stats.total_scanning_work_minor, stats.total_scanning_work_major);

#if defined(POSIX_CLOCK)
  double time_per_minor =
    average(stats.total_minor_time, stats.minor_collections) / 1000;
  double time_per_major =
    average(stats.total_major_time, stats.major_collections) / 1000;

  printf("average time per minor: %'.3fµs\n"
         "average time per major: %'.3fµs\n"
         "peak time per minor: %'.3fµs\n"
         "peak time per major: %'.3fµs\n",
         time_per_minor,
         time_per_major,
         ((double)stats.peak_minor_time) / 1000,
         ((double)stats.peak_major_time) / 1000);
#endif
}

/* }}} */

/* {{{ Hook setup */

static int setup = 0;

static void scanning_callback(scanning_action action, int only_young, void *data)
{
  if (!setup) return;
  int in_minor_collection = bxr_in_minor_collection();
  if (in_minor_collection) ++stats.minor_collections;
  else ++stats.major_collections;
  int64_t start = time_counter();
  scan_roots(action, data);
  int64_t duration = time_counter() - start;
  int64_t *total = in_minor_collection ? &stats.total_minor_time : &stats.total_major_time;
  int64_t *peak = in_minor_collection ? &stats.peak_minor_time : &stats.peak_major_time;
  *total += duration;
  if (duration > *peak) *peak = duration;
}

// Must be called to set the hook before using boxroot
int bitmap_boxroot_setup()
{
  if (setup) return 0;
  // initialise globals
  struct stats empty_stats = {0};
  stats = empty_stats;
  rings.young = NULL;
  rings.old = NULL;
  bxr_setup_hooks(&scanning_callback, NULL);
  // we are done
  setup = 1;
  if (DEBUG) validate_all_rings();
  return 1;
}

void bitmap_boxroot_teardown()
{
  if (!setup) return;
  setup = 0;
  free_ring(rings.young);
  rings.young = NULL;
  free_ring(rings.old);
  rings.old = NULL;
}

/* }}} */
