#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CAML_INTERNALS

#include "boxroot.h"
#include <caml/roots.h>
#include <caml/minor_gc.h>

// Options

#define POOL_LOG_SIZE 15 // 12 = 4KB
#define LOW_CAPACITY_THRESHOLD 50 /* 50% capacity before promoting a
                                     young pool. */
/* Print statistics on teardown? */
#define PRINT_STATS 1
/* Allocate with mmap? */
#define USE_MMAP 0
/* Advise to use transparent huge pages? (Linux)

   Little effect in my experiments. This is to investigate further:
   indeed the options "thp:always,metadata_thp:auto" for jemalloc
   consistently brings it faster than ocaml-ephemeral. FTR:
   `MALLOC_CONF="thp:always,metadata_thp:auto"                 \
   LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2       \
   make run`. (probably due to the malloc in value_list_functor.h)
*/
#define USE_MADV_HUGEPAGE 1
/* Whether to pre-allocate several pools at once. Free pools are put
   aside and re-used instead of being immediately freed. Does not
   support memory reclamation yet.
   + Visible effect for large pool sizes (independently of superblock
     size) with glibc malloc. Probably by avoiding the cost of memory
     reclamation. Note: this is fair: it is enough to reclaim memory
     at Gc.compact like the OCaml Gc. Not observed with jemalloc.
   - Slower than aligned_alloc for small pool sizes (independently of
     superblock sizes?). */
#define USE_SUPERBLOCK 1
#define SUPERBLOCK_LOG_SIZE 21 // 21 = 2MB (a huge page)
/* Defragment during scanning?
   + Better cache locality for successive allocations after lots of
     deallocations.
   + Better impact of bump pointer optimisation on scanning times after
     lots of deallocations.
   - Scanning is a lot more expensive (read-write instead of read-only).
   TODO: Option to defragment every X, or find a measure of fragmentation
   to decide when to defragment. */
#define DEFRAG 0
/* DEBUG? (slow) */
#define DEBUG 0

// for MADV_HUGEPAGE
#define HUGEPAGE_LOG_SIZE 21 // 2MB on x86_64 Linux
#if USE_MADV_HUGEPAGE != 0
  #include <sys/mman.h>
#endif


// Data types

#define CHUNK_LOG_SIZE (USE_SUPERBLOCK ? SUPERBLOCK_LOG_SIZE : POOL_LOG_SIZE)

static_assert(CHUNK_LOG_SIZE >= POOL_LOG_SIZE,
              "chunk size smaller than pool size");
static_assert(!USE_MADV_HUGEPAGE || CHUNK_LOG_SIZE >= HUGEPAGE_LOG_SIZE,
              "chunk size smaller than a huge page");

#define POOL_SIZE ((size_t)1 << POOL_LOG_SIZE)
#define CHUNK_SIZE ((size_t)1 << CHUNK_LOG_SIZE)
#define CHUNK_ALIGNMENT                                         \
  ((USE_MADV_HUGEPAGE && POOL_LOG_SIZE < HUGEPAGE_LOG_SIZE) ?   \
   ((size_t)1 << HUGEPAGE_LOG_SIZE) : POOL_SIZE)
#define POOLS_PER_CHUNK (CHUNK_SIZE / POOL_SIZE)

typedef void * slot;

struct header {
  struct pool *prev;
  struct pool *next;
  slot *free_list;
  int alloc_count;
  int capacity; /* Number of non-null slots, updated at the end of
                   each scan. */
};

#define POOL_ROOTS_CAPACITY                               \
  ((POOL_SIZE - sizeof(struct header)) / sizeof(slot) - 1)
/* &pool->roots[POOL_ROOTS_CAPACITY] can end up as a placeholder value
   in the freelist to denote the last element of the freelist,
   starting from after releasing from a full pool for the first
   time. To ensure that this value is recognised by the test
   [get_pool_header(v) == pool], we subtract one from the capacity. */

typedef struct pool {
  struct header hd;
  /* Unoccupied slots are either NULL or a pointer to the next free
     slot. The null value acts as a terminator: if a slot is null,
     then all subsequent slots are null (bump pointer optimisation). */
  slot roots[POOL_ROOTS_CAPACITY];
  uintptr_t end;// unused
} pool;

static_assert(POOL_ROOTS_CAPACITY <= INT_MAX, "capacity too large");
static_assert(sizeof(pool) == POOL_SIZE, "bad pool size");

// hot path
static inline pool * get_pool_header(slot v)
{
  return (pool *)((uintptr_t)v & ~((uintptr_t)POOL_SIZE - 1));
}

// Rings of pools
static struct {
  pool * old;
  pool * young;
  pool * free;
} pools;

typedef enum class {
  YOUNG,
  OLD,
  UNTRACKED
} class;

static struct {
  int minor_collections;
  int major_collections;
  int total_create;
  int total_delete;
  int total_modify;
  int total_scanning_work_minor;
  int total_scanning_work_major;
  int total_alloced_chunks;
  int total_alloced_pools;
  int total_freed_pools;
  int live_pools;
  int peak_pools;
  int get_available_pool_seeking;
  int ring_operations; // Number of times hd.next is mutated
  int defrag_sort;
  int defrag_shorten;
} stats; // zero-initialized


// Platform-specific

#ifdef __APPLE__
static void *aligned_alloc(size_t alignment, size_t size) {
  void *memptr = NULL;
  posix_memalign(&memptr, alignment, size);
  return memptr;
}
#endif

static void * alloc_chunk()
{
  void *p = NULL;
  if (USE_MMAP) {
    uintptr_t bitmask = ((uintptr_t)CHUNK_ALIGNMENT) - 1;
    p = mmap(0, CHUNK_SIZE + CHUNK_ALIGNMENT, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = NULL;
    // align p
    p = (void *)(((uintptr_t)p + bitmask) & ~bitmask);
    // TODO: release unused portions
  } else {
    p = aligned_alloc(CHUNK_ALIGNMENT, CHUNK_SIZE); // TODO: not portable
  }
  if (p == NULL) return NULL;
#if USE_MADV_HUGEPAGE != 0
  madvise(p, CHUNK_SIZE, MADV_HUGEPAGE);
#endif
  if (PRINT_STATS) ++stats.total_alloced_chunks;
  return p;
}

static void free_chunk(void *p)
{
  if (USE_MMAP) {
    // TODO: not implemented
    //munmap(p, POOL_SIZE);
  } else {
    free(p);
  }
}

// Ring operations

static void ring_init(pool *p)
{
  p->hd.next = p;
  p->hd.prev = p;
  if (PRINT_STATS) ++stats.ring_operations;
}

// insert the ring [source] in front of [*target]
static void ring_concat(pool *source, pool **target)
{
  if (source == NULL) return;
  pool *old = *target;
  if (old == NULL) {
    *target = source;
  } else {
    pool *last = old->hd.prev;
    last->hd.next = source;
    source->hd.prev->hd.next = old;
    old->hd.prev = source->hd.prev;
    source->hd.prev = last;
    *target = source;
    if (PRINT_STATS) stats.ring_operations += 2;
  }
}

// remove the first element from [*target] and return it
static pool * ring_pop(pool **target)
{
  pool *front = *target;
  assert(front);
  if (front->hd.next == front) {
    assert(front->hd.prev == front);
    *target = NULL;
    return front;
  }
  front->hd.prev->hd.next = front->hd.next;
  front->hd.next->hd.prev = front->hd.prev;
  if (PRINT_STATS) ++stats.ring_operations;
  *target = front->hd.next;
  ring_init(front);
  return front;
}

// Pool management

static pool * get_free_pool()
{
  if (pools.free != NULL) {
    return ring_pop(&pools.free);
  }
  pool *chunk = alloc_chunk();
  if (chunk == NULL) return NULL;
  pool *p;
  for (p = chunk + POOLS_PER_CHUNK - 1; p >= chunk; p--) {
    ring_init(p);
    ring_concat(p, &pools.free);
  }
  return ring_pop(&pools.free);
}

static pool * alloc_pool()
{
  if (PRINT_STATS) {
    ++stats.total_alloced_pools;
    ++stats.live_pools;
    if (stats.live_pools > stats.peak_pools)
      stats.peak_pools = stats.live_pools;
  }
  pool *out = get_free_pool();

  if (out == NULL) return NULL;

  out->hd.free_list = out->roots;
  out->hd.alloc_count = 0;
  out->hd.capacity = 0;
  memset(out->roots, 0, sizeof(out->roots));

  return out;
}

// Find an available pool for the class; place it in front of the ring
// and return it. Return NULL if none was found and the allocation of
// a new one failed.
static pool * get_available_pool(class class)
{
  // If there was no optimisation for immediates, we could always place
  // the immediates with the old values (be careful about NULL in
  // naked-pointers mode, though).
  pool **pool_ring = (class == YOUNG) ? &pools.young : &pools.old;
  pool *start_pool = *pool_ring;

  if (start_pool != NULL) {
    assert(start_pool->hd.alloc_count == POOL_ROOTS_CAPACITY);

    // Find a pool with available slots. TODO: maybe faster lookup by
    // putting the more empty pools in the front during scanning, and/or
    // temporarily place full ones in an auxiliary ring (until next
    // scan).
    for (pool *next_pool = start_pool->hd.next;
         next_pool != start_pool;
         next_pool = next_pool->hd.next) {
      if (PRINT_STATS) ++stats.get_available_pool_seeking;
      if (next_pool->hd.alloc_count < POOL_ROOTS_CAPACITY) {
        // Rotate the ring, making the pool with free slots the head
        *pool_ring = next_pool;
        return next_pool;
      }
    }
  }

  // None found, add a new pool at the start
  pool *new_pool = alloc_pool();
  if (new_pool == NULL) return NULL;
  ring_concat(new_pool, pool_ring);

  return new_pool;
}

// Free a pool if empty and not the last of its ring.
static void try_free_pool(pool *p)
{
  if (p->hd.alloc_count != 0 || p->hd.next == p) return;

  pool *hd = ring_pop(&p);
  if (pools.old == hd) pools.old = p;
  else if (pools.young == hd) pools.young = p;
  assert(hd != pools.free);
  if (USE_SUPERBLOCK) {
    // TODO: implement reclamation
    ring_concat(hd, &pools.free);
  } else {
    free_chunk(hd);
  }
  if (PRINT_STATS) stats.total_freed_pools++;
}

// Allocation, deallocation

// hot path
static inline value * freelist_pop(pool *p)
{
  slot *new_root = p->hd.free_list;
  slot next = *new_root;
  p->hd.alloc_count++;

  // [new_root] contains either a pointer to the next free slot or
  // NULL. If it is NULL, the next free slot is adjacent to the
  // current one.
  p->hd.free_list = (next == NULL) ? new_root+1 : (slot *)next;
  return (value *)new_root;
}

// slow path: Place an available pool in front of the ring and
// allocate from it.
static value * alloc_boxroot_slow(class class)
{
  assert(class != UNTRACKED);
  pool *p = get_available_pool(class);
  if (p == NULL) return NULL;
  assert(p->hd.alloc_count != POOL_ROOTS_CAPACITY);
  return freelist_pop(p);
}

// hot path
static inline value * alloc_boxroot(class class)
{
  // TODO Latency: bound the number of young roots alloced at each
  // minor collection by scheduling a minor collection.
  pool *p = (class == YOUNG) ? pools.young : pools.old;
  // Test for NULL is necessary here: it is not always possible to
  // allocate the first pool elsewhere, e.g. in scanning functions
  // which must not fail.
  if (p != NULL && p->hd.alloc_count != POOL_ROOTS_CAPACITY) {
    return freelist_pop(p);
  }
  return alloc_boxroot_slow(class);
}

// hot path
static inline void free_boxroot(value *root)
{
  pool *p = get_pool_header((slot)root);
  *(slot *)root = p->hd.free_list;
  p->hd.free_list = (slot)root;
  if (--p->hd.alloc_count == 0) {
    try_free_pool(p);
  }
}

// Scanning

static void (*boxroot_prev_scan_roots_hook)(scanning_action);

static int is_minor_scanning(scanning_action action)
{
  return action == &caml_oldify_one;
}

static int validate_pool(pool *pool, int do_capacity)
{
  // check capacity (needs to be up-to-date)
  if (do_capacity) {
    int i = 0;
    for (; i < POOL_ROOTS_CAPACITY; i++) {
      if (pool->roots[i] == NULL) break;
    }
    assert(pool->hd.capacity == i);
  }
  // check freelist structure and length
  slot *pool_end = &pool->roots[POOL_ROOTS_CAPACITY];
  slot *curr = pool->hd.free_list;
  int length = 0;
  while (curr != pool_end) {
    length++;
    assert(length <= POOL_ROOTS_CAPACITY);
    assert(curr >= pool->roots && curr < pool_end);
    slot s = *curr;
    if (s == NULL) {
      for (int i = curr - pool->roots + 1; i < POOL_ROOTS_CAPACITY; i++) {
        length++;
        assert(pool->roots[i] == NULL);
      }
      break;
    }
    curr = (slot *)s;
  }
  assert(length == POOL_ROOTS_CAPACITY - pool->hd.alloc_count);
  // check count of allocated elements
  int alloc_count = 0;
  for(int i = 0; i < POOL_ROOTS_CAPACITY; i++) {
    slot v = pool->roots[i];
    if (v == NULL) break;
    if (get_pool_header(v) != pool) {
      ++alloc_count;
    }
  }
  assert(alloc_count == pool->hd.alloc_count);
}

static int scan_pool(scanning_action action, pool * pool)
{
  slot *current = pool->roots;
  slot *pool_end = &pool->roots[POOL_ROOTS_CAPACITY];
  /* For DEFRAG */
  int capacity = 0;
  int allocs_to_find = pool->hd.alloc_count;
  slot **freelist_last = &pool->hd.free_list;
  slot *freelist_next = NULL;
  for (; current != pool_end; ++current) {
    // hot path
    slot v = *current;
    if (v == NULL) {
      // We can skip the rest if the pointer value is NULL.
      if (DEFRAG && DEBUG) assert(allocs_to_find == 0);
      break;
    }
    if (get_pool_header(v) != pool) {
      // The value is an OCaml block (or possibly an immediate whose
      // msbs differ from those of [pool], if the immediates
      // optimisation were to be turned off).
      (*action)((value)v, (value *) current);
      if (DEFRAG && --allocs_to_find == 0) capacity = current - pool->roots + 1;
    } else if (DEFRAG) {
      // Current slot is non-allocated (requires optimisation for
      // immediates to avoid false positives).
      if (allocs_to_find == 0) {
        // Past the last allocation: set remaining to zero
        for (; current != pool_end; ++current) {
          if (*current == NULL) break;
          *current = NULL;
          if (PRINT_STATS) ++stats.defrag_shorten;
        }
        break;
      } else {
        // An element of the freelist. Sort the freelist and record
        // the freelist_last.
        if (freelist_next == NULL) freelist_last = (slot **)current;
        *current = (slot)freelist_next;
        freelist_next = current;
        if (PRINT_STATS) ++stats.defrag_sort;
      }
    }
  }
  int work = current - pool->roots;
  if (DEFRAG) {
    // Now we know what is the first element of the freelist and where
    // last element of the freelist points.
    pool->hd.free_list = freelist_next;
    *freelist_last = &pool->roots[capacity];
    pool->hd.capacity = capacity;
  } else {
    pool->hd.capacity = current - pool->roots;
  }
  return work;
}

static int scan_pools(scanning_action action, pool *start_pool)
{
  int work = 0;
  if (start_pool == NULL) return work;
  work += scan_pool(action, start_pool);
  if (DEBUG) validate_pool(start_pool, 1);
  for (pool *pool = start_pool->hd.next;
       pool != start_pool;
       pool = pool->hd.next) {
    work += scan_pool(action, pool);
    if (DEBUG) validate_pool(pool, 1);
  }
  return work;
}

static void scan_for_minor(scanning_action action)
{
  ++stats.minor_collections;
  if (pools.young == NULL) return;
  int work = scan_pools(action, pools.young);
  stats.total_scanning_work_minor += work;
  // promote minor pools
  pool *new_young_pool = NULL;
  if ((pools.young->hd.capacity * 100 / POOL_ROOTS_CAPACITY) <=
      LOW_CAPACITY_THRESHOLD) {
    new_young_pool = ring_pop(&pools.young);
  }
  ring_concat(pools.young, &pools.old);
  pools.young = new_young_pool;
}

static void scan_for_major(scanning_action action)
{
  ++stats.major_collections;
  int work = scan_pools(action, pools.young);
  work += scan_pools(action, pools.old);
  stats.total_scanning_work_major += work;
}

static void boxroot_scan_roots(scanning_action action)
{
  if (is_minor_scanning(action)) {
    scan_for_minor(action);
  } else {
    scan_for_major(action);
  }
  if (boxroot_prev_scan_roots_hook != NULL) {
    (*boxroot_prev_scan_roots_hook)(action);
  }
}

// 1=KiB, 2=MiB
static int kib_of_pools(int count, int unit)
{
  int log_per_pool = POOL_LOG_SIZE - unit * 10;
  if (log_per_pool >= 0) return count << log_per_pool;
  if (log_per_pool < 0) return count >> -log_per_pool;
}

static int average(int total_work, int nb_collections) {
    if (nb_collections <= 0)
        return -1;
    // round to nearest
    return ((total_work + (nb_collections / 2)) / nb_collections);
}

static void print_stats()
{
  printf("minor collections: %d\n"
         "major collections: %d\n",
         stats.minor_collections,
         stats.major_collections);

  int scanning_work_minor = average(stats.total_scanning_work_minor, stats.minor_collections);
  int scanning_work_major = average(stats.total_scanning_work_major, stats.major_collections);
  int total_scanning_work = stats.total_scanning_work_minor + stats.total_scanning_work_major;
  int ring_operations_per_pool = average(stats.ring_operations, stats.total_alloced_pools);
  int total_mib = kib_of_pools(stats.total_alloced_pools, 2);
  int freed_mib = kib_of_pools(stats.total_freed_pools, 2);
  int peak_mib = kib_of_pools(stats.peak_pools, 2);

  if (total_scanning_work == 0 && stats.total_alloced_pools == 0)
    return;

  printf("POOL_LOG_SIZE: %d (%d KiB, %d roots)\n"
         "LOW_CAPACITY_THRESHOLD: %d%%\n"
         "USE_MMAP: %d\n"
         "USE_MADV_HUGEPAGE: %d\n"
         "USE_SUPERBLOCK: %d\n"
         "SUPERBLOCK_LOG_SIZE: %d\n"
         "DEFRAG: %d\n"
         "DEBUG: %d\n",
         (int)POOL_LOG_SIZE, kib_of_pools((int)1, 1), (int)POOL_ROOTS_CAPACITY,
         (int)LOW_CAPACITY_THRESHOLD,
         (int)USE_MMAP,
         (int)USE_MADV_HUGEPAGE,
         (int)USE_SUPERBLOCK,
         (int)SUPERBLOCK_LOG_SIZE,
         (int)DEFRAG,
         (int)DEBUG);

  printf("CHUNK_SIZE: %d kiB (%d pools)\n"
         "CHUNK_ALIGNMENT: %d kiB\n"
         "total allocated chunks: %d (%d pools)\n",
         kib_of_pools(POOLS_PER_CHUNK,1), (int)POOLS_PER_CHUNK,
         kib_of_pools(CHUNK_ALIGNMENT / POOL_SIZE,1),
         stats.total_alloced_chunks, stats.total_alloced_chunks * (int)POOLS_PER_CHUNK);

  printf("total allocated pools: %d (%d MiB)\n"
         "total freed pools: %d (%d MiB)\n"
         "peak allocated pools: %d (%d MiB)\n"
         "get_available_pool seeking: %d\n",
         stats.total_alloced_pools, total_mib,
         stats.total_freed_pools, freed_mib,
         stats.peak_pools, peak_mib,
         stats.get_available_pool_seeking);

  printf("work per minor: %d\n"
         "work per major: %d\n"
         "total scanning work: %d\n",
         scanning_work_minor,
         scanning_work_major,
         total_scanning_work);

  printf("total created: %d\n"
         "total deleted: %d\n"
         "total modified: %d\n",
         stats.total_create,
         stats.total_delete,
         stats.total_modify);

  printf("total ring operations: %d\n"
         "ring operations per pool: %d\n",
         stats.ring_operations,
         ring_operations_per_pool);

  printf("defrag (sort): %d\n"
         "defrag (shorten): %d\n",
         stats.defrag_sort,
         stats.defrag_shorten);
}

// Must be called to set the hook
value boxroot_scan_hook_setup(value unit)
{
  boxroot_prev_scan_roots_hook = caml_scan_roots_hook;
  caml_scan_roots_hook = boxroot_scan_roots;
  return Val_unit;
}

static void force_free_pools(pool *start)
{
  if (USE_SUPERBLOCK) {
    // TODO: not implemented
    return;
  }
  if (start == NULL) return;
  pool *p = start;
  do {
    pool *next = p->hd.next;
    free_chunk(p);
    p = next;
  } while (p != start);
}

value boxroot_scan_hook_teardown(value unit)
{
  caml_scan_roots_hook = boxroot_prev_scan_roots_hook;
  boxroot_prev_scan_roots_hook = NULL;
  if (PRINT_STATS) print_stats();
  force_free_pools(pools.young);
  force_free_pools(pools.old);
  return Val_unit;
}

// Boxroot API implementation

// hot path
static inline class classify_value(value v)
{
  if(v == 0 || !Is_block(v)) return UNTRACKED;
  if(Is_young(v)) return YOUNG;
  return OLD;
}

// hot path
static inline class classify_boxroot(boxroot root)
{
    return classify_value(*(value *)root);
}

// hot path
static inline boxroot boxroot_create_classified(value init, class class)
{
  if (PRINT_STATS) ++stats.total_create;
  value *cell;
  if (class != UNTRACKED) {
    cell = alloc_boxroot(class);
  } else {
    // [init] can be null in naked-pointers mode, handled here.
    cell = (value *) malloc(sizeof(value));
    // TODO: further optim: use a global table instead of malloc for
    // very small values of [init] — for fast variants and and to
    // handle C-side NULLs in no-naked-pointers mode if desired.
  }
  if (cell != NULL) *cell = init;
  return (boxroot)cell;
}

boxroot boxroot_create(value init)
{
  return boxroot_create_classified(init, classify_value(init));
}

value const * boxroot_get(boxroot root)
{
  return (value *)root;
}

// hot path
static inline void boxroot_delete_classified(boxroot root, class class)
{
  if (PRINT_STATS) ++stats.total_delete;
  value *cell = (value *)root;
  if (class != UNTRACKED) {
    free_boxroot(cell);
  } else {
    free(cell);
  }
}

void boxroot_delete(boxroot root)
{
  assert(root);
  boxroot_delete_classified(root, classify_boxroot(root));
}

void boxroot_modify(boxroot *root, value new_value)
{
  assert(*root);
  if (PRINT_STATS) ++stats.total_modify;
  class old_class = classify_boxroot(*root);
  class new_class = classify_value(new_value);

  if (old_class == new_class
      || (old_class == YOUNG && new_class == OLD)) {
    // No need to reallocate
    value *cell = (value *)*root;
    *cell = new_value;
    return;
  }

  boxroot_delete_classified(*root, old_class);
  *root = boxroot_create_classified(new_value, new_class);
  // Note: *root can be NULL, which must be checked (in Rust, check
  // and panic here).
}
