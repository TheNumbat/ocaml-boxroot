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
#include <stdatomic.h>

#define CAML_NAME_SPACE
#define CAML_INTERNALS

#include "rem_boxroot.h"
#include <caml/minor_gc.h>
#include <caml/major_gc.h>
#include <caml/domain_state.h>

#if defined(_POSIX_TIMERS) && defined(_POSIX_MONOTONIC_CLOCK)
#define POSIX_CLOCK
#include <time.h>
#endif

#include "ocaml_hooks.h"
#include "platform.h"

#define LIKELY(a) BXR_LIKELY(a)
#define UNLIKELY(a) BXR_UNLIKELY(a)

/* }}} */

/* {{{ Parameters */

/* Log of the size of the pools (12 = 4KB, an OS page).
   Recommended: 14. */
#define POOL_LOG_SIZE 14
#define POOL_SIZE ((size_t)1 << POOL_LOG_SIZE)

/* }}} */

/* {{{ Data types */

/* Our main data structure is a doubly-linked list of "pools"
   containing registered boxroots. Allocating boxroots in pools
   amortizes malloc() calls and improves scanning memory locality.

   Pools are allocated on aligned addresses, which gives us a fast
   way to get the owning pool of a boxroot on deletion.

   The alloc_count field is used to track the number of boxroots in
   each pool.
*/

typedef uintptr_t raw_slot;
typedef value full_slot;
typedef uintptr_t free_slot;
typedef union { raw_slot raw; free_slot free; full_slot full; } slot;
/* Each pool has a contiguous array whose elements are either:
   - registered roots (pointers to OCaml values)
   - free slots

   Registered roots are either in the major heap, in the minor heap,
   or immediates. The slot of minor-heap values must also belong to
   the remembered set.

   Free slots have their low bit set to look like immdiates.
   They form two disjoint linked lists:
   - the "major" free list which may contain any value
   - the "minor" free list whose slots are already part of the
     remembered set

   Free slots can be distinguished from full slots: pointers into the pool
   itself may only be free slots, as they are not valid OCaml values.

   When a minor collection happens, no scanning needs to be done as
   the GC already traverses the remembered set. We just add the
   minor-free-list slots to the major free list.
*/

struct header {
  struct pool *prev;
  struct pool *next;
  slot *major_free_list;
  slot *minor_free_list;
  slot *last_minor_free_slot;
  // invariant: (!is_empty_free_list(minor_free_list) => (last_minor_free_slot != NULL)
  int alloc_count;
};

static_assert(POOL_SIZE / sizeof(slot) <= INT_MAX, "pool size too large");

#define POOL_ROOTS_CAPACITY                                 \
  ((int)((POOL_SIZE - sizeof(struct header)) / sizeof(slot)))

typedef struct pool {
  struct header hd;
  slot roots[POOL_ROOTS_CAPACITY];
} pool;

static_assert(sizeof(pool) == POOL_SIZE, "bad pool size");

/* }}} */

/* {{{ Globals */

/* Global pool ring. */
static pool *pools = NULL;

/* On-the-side ring of pools that were found to be full
   by find_available_pool(). */
static pool *full_pools = NULL;

/* Global mutex */
#if defined(ENABLE_BOXROOT_MUTEX) && (ENABLE_BOXROOT_MUTEX == 1)
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#define CRITICAL_SECTION_BEGIN() pthread_mutex_lock(&mutex)
#define CRITICAL_SECTION_END() pthread_mutex_unlock(&mutex)
#else
#define CRITICAL_SECTION_BEGIN()
#define CRITICAL_SECTION_END()
#endif

struct stats {
  int minor_collections;
  int major_collections;
  atomic_int total_create;
  atomic_int total_delete;
  atomic_int total_modify;
  long long total_scanning_work; // number of slots scanned (including free slots)
  long long useful_scanning_work; // number of non-free slots scanned
  int64_t total_major_time;
  int64_t peak_major_time;
  int total_alloced_pools;
  int total_freed_pools;
  int live_pools; // number of tracked pools
  int peak_pools; // max live pools at any time
  int ring_operations; // Number of times hd.next is mutated
  long long is_young; // number of times is_young was called
  long long get_pool_header; // number of times get_pool_header was called
  long long is_free_slot; // number of times is_free_slot was called
  long long is_empty_free_list; // number of times is_empty_free_list was called
  long long remember; // number of minor values added to the remembered set
  long long find_available_pool; // number of times find_available_pool was called
  long long find_available_pool_work; // total work of find_available_pool was called
};

static struct stats stats;

/* }}} */

/* {{{ Tests in the hot path */

// hot path
static inline pool * get_pool_header(slot *v)
{
  if (DEBUG) ++stats.get_pool_header;
  return (pool *)((uintptr_t)v & ~((uintptr_t)POOL_SIZE - 1));
}

// Return true iff v has the lsb tagged and shares the same msbs as p
// hot path
static inline int is_free_slot(raw_slot v, pool *p)
{
  if (DEBUG) ++stats.is_free_slot;
  return ((uintptr_t)p | 1) == (v & (~((uintptr_t)POOL_SIZE - 2)));
}

// hot path
static inline int is_empty_free_list(slot *v, pool *p)
{
  if (DEBUG) ++stats.is_empty_free_list;
  return ((uintptr_t) v == (uintptr_t)p);
}

// hot path
static inline int is_young_block(value v)
{
  if (DEBUG) ++stats.is_young;
  return Is_block(v) && Is_young(v);
}

// hot path
static inline void remember(caml_domain_state *dom_st, slot *s)
{
  if (DEBUG) ++stats.remember;
  Add_to_ref_table(dom_st, &(s->full));
}

/* }}} */

/* {{{ Ring operations */

static void ring_link(pool *p, pool *q)
{
  p->hd.next = q;
  q->hd.prev = p;
  ++stats.ring_operations;
}

// insert the ring [source] at the back of [*target].
static void ring_push_back(pool *source, pool **target)
{
  if (source == NULL) return;
  if (*target == NULL) {
    *target = source;
  } else {
    pool *target_last = (*target)->hd.prev;
    pool *source_last = source->hd.prev;
    ring_link(target_last, source);
    ring_link(source_last, *target);
  }
}

// remove the first element from [*target] and return it
static pool * ring_pop(pool **target)
{
  pool *front = *target;
  assert(front);
  if (front->hd.next == front) {
    *target = NULL;
    return front;
  }
  ring_link(front->hd.prev, front->hd.next);
  *target = front->hd.next;
  ring_link(front, front);
  return front;
}

/* }}} */

/* {{{ Free list management */

static inline int is_valid_free_slot(slot *s) {
  pool *p = get_pool_header(s);
  return (is_empty_free_list(s, p) || is_free_slot(s->raw, p));
}

static inline slot *untag_free_slot(free_slot v) {
    slot *s = (slot*) ((v | 1) - 1);
    DEBUGassert(is_valid_free_slot(s));
    return s;
}

static inline free_slot tag_free_slot(slot *s) {
    DEBUGassert(is_valid_free_slot(s));
    return (((uintptr_t) s) | 1);
}

static inline void free_list_push(slot *s, slot **free_list) {
  s->free = tag_free_slot(*free_list);
  *free_list = s;
}

static inline slot *free_list_pop(slot **free_list) {
  DEBUGassert (!is_empty_free_list(*free_list, get_pool_header(*free_list)));
  slot *s = *free_list;
  DEBUGassert(is_free_slot(s->raw, get_pool_header(s)));
  *free_list = untag_free_slot(s->free);
  return s;
}

/* }}} */

/* {{{ Pool management */

// the empty free-list for a pool p is denoted by a pointer to the pool itself
// (NULL could be a valid value for an element slot)
static inline slot *empty_free_list(pool *p) {
  return (slot*)p;
}

static inline int is_full_pool(pool *p)
{
  // we could also check is_empty_free_list(p->hd.free_list, p)
  return (p->hd.alloc_count == POOL_ROOTS_CAPACITY);
}

static inline int is_empty_pool(pool *p)
{
  return (p->hd.alloc_count == 0);
}

static inline int is_almost_full_pool(pool *p)
{
  return (p->hd.alloc_count > POOL_ROOTS_CAPACITY * 3 / 4);
}

static pool * get_empty_pool(void)
{
  ++stats.live_pools;
  if (stats.live_pools > stats.peak_pools) stats.peak_pools = stats.live_pools;

  pool *p = bxr_alloc_uninitialised_pool(POOL_SIZE);
  if (p == NULL) return NULL;
  ++stats.total_alloced_pools;

  ring_link(p, p);
  p->hd.major_free_list = empty_free_list(p);
  p->hd.minor_free_list = empty_free_list(p);
  p->hd.last_minor_free_slot = NULL;
  p->hd.alloc_count = 0;

  /* Put all the pool elements in its free list. */
  for (slot *s = p->roots + POOL_ROOTS_CAPACITY - 1; s >= p->roots; --s) {
    free_list_push(s, &(p->hd.major_free_list));
  }

  return p;
}

/* Find an available non-full pool or allocate a new one, ensure it is at the
   start of the ring of pools, and return it.

   Full pools encountered are moved to the full_pools ring.

   Returns NULL if none was found and the allocation of a new
   one failed. */
static pool * find_available_pool(void)
{
  if (DEBUG) stats.find_available_pool++;
  while (pools != NULL && is_full_pool(pools)) {
    if (DEBUG) stats.find_available_pool_work++;
    ring_push_back(ring_pop(&pools), &full_pools);
  }
  if (pools == NULL) {
    if (DEBUG) stats.find_available_pool_work++;
    pools = get_empty_pool();
    if (pools == NULL) return NULL;
  }
  if (DEBUG) stats.find_available_pool_work++;
  assert(pools != NULL && !is_full_pool(pools));
  return pools;
}

// remove the given pool from its global pool ring
static pool *pool_remove(pool *p)
{
  pool *removed = ring_pop(&p);
  if (removed == pools) pools = p;
  if (removed == full_pools) full_pools = p;
  return removed;
}

static void free_all_pools(void) {
  while (pools != NULL) {
    pool *p = ring_pop(&pools);
    bxr_free_pool(p);
    ++stats.total_freed_pools;
  }
}

/* }}} */

/* {{{ Allocation, deallocation */

static int setup;

// fails if the front pool is full
// hot path
static inline slot * alloc_slot(int for_young)
{
  pool *p = pools;
  if (UNLIKELY(p == NULL || is_full_pool(p))) {
    // We might be here because boxroot is not setup.
    if (!setup) {
      fprintf(stderr, "boxroot is not setup\n");
      return NULL;
    }
    p = find_available_pool();
    if (p == NULL) return NULL;
    assert(!is_full_pool(p));
  }
  p->hd.alloc_count++;
  if (for_young) {
    if (LIKELY(!is_empty_free_list(p->hd.minor_free_list, p))) {
      return free_list_pop(&(p->hd.minor_free_list));
    } else {
      // take a major slot, add it to the remembered set
      slot *new_slot = free_list_pop(&(p->hd.major_free_list));
      remember(Caml_state, new_slot);
      return new_slot;
    }
  } else {
    if (LIKELY(!is_empty_free_list(p->hd.major_free_list, p))) {
      return free_list_pop(&(p->hd.major_free_list));
    } else {
      /* If there are minor slots available, but no major slots left, we
         just reuse a minor slot, forgetting that it is in the
         remembered set.

         Note: we could also look for another pool with minor slots
         left, but we prefer to keep the minor free list as a pool-local
         optimization. Looking for another pool can degrade performance
         (if we have to look over all pools without finding anything)
         and we don't know of a good, simple strategy to avoid them.
      */
      return free_list_pop(&(p->hd.minor_free_list));
    }
  }
}

// hot path
static inline void dealloc_slot(slot *v) {
  pool *p = get_pool_header(v);
  if (!is_young_block(v->full)) {
    free_list_push(v, &(p->hd.major_free_list));
  } else {
    /* If the performance of the branch below matters, then we are in
       the case where many young boxroots are deleted, so the check is
       UNLIKELY. */
    if (UNLIKELY(is_empty_free_list(p->hd.minor_free_list, p))) {
      p->hd.last_minor_free_slot = v;
    }
    free_list_push(v, &(p->hd.minor_free_list));
  }
  p->hd.alloc_count--;
  if (UNLIKELY(p->hd.alloc_count == POOL_ROOTS_CAPACITY * 3 / 4)) {
    // the pool at this point is either in 'pools' or 'full_pools';
    // ensure that it goes back to 'pools' in any case.
    if (DEBUG) stats.find_available_pool_work++;
    pool *removed = pool_remove(p);
    ring_push_back(removed, &pools);
  }
}

/* }}} */

/* {{{ Boxroot API implementation */

// hot path
rem_boxroot rem_boxroot_create(value init)
{
  if (DEBUG) ++stats.total_create;
  CRITICAL_SECTION_BEGIN();
  slot *cell = alloc_slot(is_young_block(init));
  CRITICAL_SECTION_END();
  if (UNLIKELY(cell == NULL)) return NULL;
  cell->full = init;
  return (rem_boxroot)cell;
}

extern value rem_boxroot_get(rem_boxroot root);
extern value const * rem_boxroot_get_ref(rem_boxroot root);

// hot path
void rem_boxroot_delete(rem_boxroot root)
{
  if (DEBUG) ++stats.total_delete;
  slot *cell = (slot *)root;
  DEBUGassert(cell != NULL);
  CRITICAL_SECTION_BEGIN();
  dealloc_slot(cell);
  CRITICAL_SECTION_END();
}

// hot path
void rem_boxroot_modify(rem_boxroot *root, value new_value)
{
  if (DEBUG) ++stats.total_modify;
  slot *cell = (slot *)*root;
  DEBUGassert(cell != NULL);
  // no need for a critical section here, we do not touch the pool structure
  if (!is_young_block(new_value)) {
    cell->full = new_value;
  } else {
    value old_value = cell->full;
    cell->full = new_value;
    if (!is_young_block(old_value)) remember(Caml_state, cell);
  }
}

/* }}} */

/* {{{ Scanning */

static void validate_roots(pool *p, long *out_free_count, long *out_full_count) {
  long free_count = 0;
  long full_count = 0;
  for (slot *s = p->roots; s < p->roots + POOL_ROOTS_CAPACITY; ++s) {
    if (is_free_slot(s->raw, p)) free_count++;
    else full_count++;
  }
  *out_free_count += free_count;
  *out_full_count += full_count;
}

static void validate_free_list(pool *p, slot *free_list, long *out_free_list_count) {
  long count = 0;
  for (slot *s = free_list;
       !is_empty_free_list(s, p);
       s = untag_free_slot(s->free)) {
    assert(Is_long(s->full));
    assert(is_free_slot(s->raw, p));
    ++count;
  }
  *out_free_list_count += count;
}

static void validate_pool(pool *p) {
  long free_roots_count = 0, full_roots_count = 0, free_list_count = 0;
  validate_roots(p, &free_roots_count, &full_roots_count);
  validate_free_list(p, p->hd.major_free_list, &free_list_count);
  validate_free_list(p, p->hd.minor_free_list, &free_list_count);
  assert(is_empty_free_list(p->hd.minor_free_list, p)
         || (p->hd.last_minor_free_slot != NULL
             && is_free_slot(p->hd.last_minor_free_slot->raw, p)));
  assert(free_roots_count == free_list_count);
  assert(full_roots_count == p->hd.alloc_count);
  assert(free_roots_count + full_roots_count == POOL_ROOTS_CAPACITY);
}

static void validate_pool_ring(pool *first_pool) {
  if (first_pool == NULL) return;
  pool *p = first_pool;
  do {
    validate_pool(p);
    p = p->hd.next;
  } while (p != first_pool);
}

static void validate(void)
{
  validate_pool_ring(pools);
  validate_pool_ring(full_pools);
}

static void scan_pool(scanning_action action, void *data, pool *p)
{
  if (bxr_in_minor_collection()) {
      /* We use the remembered set for minor boxroots,
         so no scanning is necesary on minor collections.

         The remembered set is cleared by the minor collection, so the
         "minor" free list slots must now be moved to the major free
         list. */
      if (!is_empty_free_list(p->hd.minor_free_list, p)) {
        assert(p->hd.last_minor_free_slot != NULL);
        assert(is_free_slot(p->hd.minor_free_list->raw, p));
        assert(is_empty_free_list(untag_free_slot(p->hd.last_minor_free_slot->free), p));
        p->hd.last_minor_free_slot->free = tag_free_slot(p->hd.major_free_list);
        p->hd.major_free_list = p->hd.minor_free_list;
        p->hd.minor_free_list = empty_free_list(p);
      }
      return;
  } else {
    int allocs_to_find = p->hd.alloc_count;
    stats.useful_scanning_work += p->hd.alloc_count;
    for (slot *current = p->roots;
         current < p->roots + POOL_ROOTS_CAPACITY;
         ++current) {
      if (allocs_to_find == 0) {
        stats.total_scanning_work += (current - p->roots);
        return;
      }
      if (!is_free_slot(current->raw, p)) {
        /* we only scan in the major collection,
           after young blocks have been oldified */
        DEBUGassert(!is_young_block(current->full));
        --allocs_to_find;
        CALL_GC_ACTION(action, data, current->full, &(current->full));
      }
    }
    assert(allocs_to_find == 0);
    stats.total_scanning_work += POOL_ROOTS_CAPACITY;
  }
}

static void scan_pool_ring(scanning_action action, void *data, pool *first_pool)
{
  if (first_pool == NULL) return;
  pool *p = first_pool;
  do {
    scan_pool(action, data, p);
    p = p->hd.next;
  } while (p != first_pool);
}

static void free_empty_pools(void) {
  /* We don't scan the full-pool ring, whose pools are almost-full. */
  pool *p = pools;
  /* We free all empty pools except one, to avoid stuttering effects. */
  int keep_empty_pools = 1;
  do {
    pool *next = p->hd.next;
    if (is_empty_pool(p)) {
      if (keep_empty_pools > 0) {
        --keep_empty_pools;
      } else {
        bxr_free_pool(pool_remove(p));
        ++stats.total_freed_pools;
      }
    }
    p = next;
  } while (p != pools);
}

static void scan_roots(scanning_action action, void *data)
{
  if (DEBUG) validate();
  scan_pool_ring(action, data, pools);
  scan_pool_ring(action, data, full_pools);
  free_empty_pools();
  if (DEBUG) validate();
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

// 1=KiB, 2=MiB
static int kib_of_pools(int count, int unit)
{
  int log_per_pool = POOL_LOG_SIZE - unit * 10;
  if (log_per_pool >= 0) return count << log_per_pool;
  /* log_per_pool < 0) */
  return count >> -log_per_pool;
}

static int average(long long total_work, int nb_collections)
{
  if (nb_collections <= 0) return -1;
  // round to nearest
  return (total_work + (nb_collections / 2)) / nb_collections;
}

static int boxroot_used()
{
  return (pools != NULL || stats.ring_operations > 0);
}

void rem_boxroot_print_stats()
{
  printf("minor collections: %d\n"
         "major collections (and others): %d\n",
         stats.minor_collections,
         stats.major_collections);

  int scanning_work = average(stats.total_scanning_work, stats.major_collections);
  int useful_scanning_work = average(stats.useful_scanning_work, stats.major_collections);
  int ring_operations_per_pool = average(stats.ring_operations, stats.total_alloced_pools);

  if (!boxroot_used()) return;

  int64_t time_per_major =
      stats.major_collections ? stats.total_major_time / stats.major_collections : 0;

  printf("POOL_LOG_SIZE: %d (%'d KiB, %'d roots/pool)\n"
         "DEBUG: %d\n"
         "OCAML_MULTICORE: %d\n",
         (int)POOL_LOG_SIZE, kib_of_pools((int)1, 1), (int)POOL_ROOTS_CAPACITY,
         (int)DEBUG, (int)OCAML_MULTICORE);

  printf("total allocated pool: %'d (%'d MiB)\n"
         "peak allocated pools: %'d (%'d MiB)\n"
         "total freed pool: %'d (%'d MiB)\n",
         stats.total_alloced_pools,
         kib_of_pools(stats.total_alloced_pools, 2),
         stats.peak_pools,
         kib_of_pools(stats.peak_pools, 2),
         stats.total_freed_pools,
         kib_of_pools(stats.total_freed_pools, 2));

  printf("work per major: %'d (%'d useful)\n"
         "total scanning work: %'lld (%'lld%% useful)\n",
         scanning_work, useful_scanning_work,
         stats.total_scanning_work,
         stats.total_scanning_work == 0
           ? 100
           : stats.useful_scanning_work * 100 / stats.total_scanning_work);

#if defined(POSIX_CLOCK)
  printf("average time per major: %'lldns\n"
         "peak time per major: %'lldns\n",
         (long long)time_per_major,
         (long long)stats.peak_major_time);
#endif

  printf("total ring operations: %'d\n"
         "ring operations per pool: %'d\n",
         stats.ring_operations,
         ring_operations_per_pool);

#if DEBUG != 0
  printf("total created: %'d\n"
         "total deleted: %'d\n"
         "total modified: %'d\n",
         stats.total_create,
         stats.total_delete,
         stats.total_modify);

  printf("is_young_block: %'lld\n"
         "get_pool_header: %'lld\n"
         "is_free_slot: %'lld\n"
         "is_empty_free_list: %'lld\n"
         "remember: %'lld\n",
         stats.is_young,
         stats.get_pool_header,
         stats.is_free_slot,
         stats.is_empty_free_list,
         stats.remember);

  printf("find_available_pool: %'lld\n"
         "find_available_pool_work: %'lld\n"
         "roots created per pool work: %'lld\n"
         , stats.find_available_pool
         , stats.find_available_pool_work
         , stats.total_create / stats.find_available_pool_work
      );
#endif
}

/* }}} */

/* {{{ Hook setup */

static int setup = 0;

static void scanning_callback(scanning_action action, int only_young,
                              void *data)
{
  CRITICAL_SECTION_BEGIN();
  if (!setup) {
    CRITICAL_SECTION_END();
    return;
  }
  int in_minor_collection = bxr_in_minor_collection();

  if (in_minor_collection) ++stats.minor_collections;
  else ++stats.major_collections;

  // If no boxroot has been allocated, then scan_roots should not have
  // any noticeable cost. For experimental purposes, since this hook
  // is also used for other the statistics of other implementations,
  // we further make sure of this with an extra test, by avoiding
  // calling scan_roots if it has only just been initialised.
  if (boxroot_used()) {
    int64_t start = time_counter();
    scan_roots(action, data);
    int64_t duration = time_counter() - start;
    int64_t *total = &stats.total_major_time;
    int64_t *peak = &stats.peak_major_time;
    *total += duration;
    if (duration > *peak) *peak = duration;
  }
  CRITICAL_SECTION_END();
}

// Must be called to set the hook before using boxroot
int rem_boxroot_setup()
{
  CRITICAL_SECTION_BEGIN();
  if (setup) {
    CRITICAL_SECTION_END();
    return 0;
  }
  // initialise globals
  struct stats empty_stats = {0};
  stats = empty_stats;
  pools = NULL;
  full_pools = NULL;
  bxr_setup_hooks(&scanning_callback, NULL);
  // we are done
  setup = 1;
  CRITICAL_SECTION_END();
  return 1;
}

// This can only be called at OCaml shutdown
void rem_boxroot_teardown()
{
  CRITICAL_SECTION_BEGIN();
  if (!setup) {
    CRITICAL_SECTION_END();
    return;
  }
  setup = 0;
  free_all_pools();
  CRITICAL_SECTION_END();
}

/* }}} */

/* {{{ */
/* }}} */
