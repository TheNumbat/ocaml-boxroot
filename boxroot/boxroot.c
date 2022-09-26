/* SPDX-License-Identifier: MIT */
/* {{{ Includes */

// This is emacs folding-mode

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CAML_NAME_SPACE
#define CAML_INTERNALS

#include "boxroot.h"
#include <caml/minor_gc.h>
#include <caml/major_gc.h>

#if defined(_POSIX_TIMERS) && defined(_POSIX_MONOTONIC_CLOCK)
#define POSIX_CLOCK
#include <time.h>
#endif

#include "ocaml_hooks.h"
#include "platform.h"

/* }}} */

/* {{{ Data types */

typedef enum class {
  YOUNG,
  OLD,
  UNTRACKED
} class;

typedef void * slot;

typedef struct pool {
  /* Free list, protected by domain lock. */
  boxroot_fl free_list;
  /* Delayed free list, protected by pool_rings lock of domain_id. */
  boxroot_fl delayed_fl;
  /* protected by pool_rings lock of domain_id, kept in sync with its
     location in the pool rings. TODO: atomic instead? */
  class class;
  /* protected by pool_rings lock of domain_id */
  struct pool *prev;
  struct pool *next;
  /* Occupied slots are OCaml values.
     Unoccupied slots are a pointer to the next slot in the free list,
     or to the pool itself, denoting the empty free list. */
  /* Each cell has an owner. The owner of a boxroot is the owner of an
     allocated cell. The domain that owns the pool owns the
     unallocated cells. Synchronisation:
     - on the domain that owns the pool, the domain lock protects the
       cells owned (transitively) by the domain.
     - other cells are protected by the pool rings mutex.
  */
  slot roots[];
} pool;

#define POOL_CAPACITY ((int)((POOL_SIZE - sizeof(pool)) / sizeof(slot)))

static_assert(POOL_SIZE / sizeof(slot) <= INT_MAX, "pool size too large");
static_assert(POOL_CAPACITY >= 1, "pool size too small");

/* }}} */

/* {{{ Globals */

/* Global pool rings. */
/* TODO: Synchronise via domain lock only. For this remove access inside
   boxroot_modify? */
typedef struct {
  /* This mutex protects:
     - rings below
     - pool cells in the above pools that are not owned by the domain. */
  mutex_t mutex;
  /* Pool of old values: contains only roots pointing to the major
     heap. Scanned at the start of major collection. */
  pool *old;
  /* Pool of young values: contains roots pointing to the major or to
     the minor heap. Scanned at the start of minor and major
     collection. */
  pool *young;
  /* Current pool. Ring of size 1. Scanned at the start of minor and
     major collection. */
  pool *current;
  /* Pools containing no root: not scanned.
     We could free these pools immediately, but this could lead to
     stuttering behavior for workloads that regularly come back to
     0 boxroots alive. Instead we wait for the next major root
     scanning to free empty pools. */
  pool *free;
} pool_rings;

/* Constant once allocated. Uses dependency ordering to publish the
   initialized value (avoid?). (TODO: simplify: separate orphaned) */
static pool_rings *pools[Num_domains + 1] = { NULL };
#define Orphaned_id Num_domains

static boxroot_fl empty_fl =
  { (slot)&empty_fl
    , NULL
    , -1
#if OCAML_MULTICORE
    , -1
#endif
  };

/* Synchronisation: via domain lock */
boxroot_fl *boxroot_current_fl[Num_domains + 1];

/* requires domain lock: NO
   requires pool lock: NO */
static inline int dom_id_of_pool(pool *p)
{
  return dom_id_of_fl(&p->free_list);
}

#if OCAML_MULTICORE
/* requires domain lock: NO
   requires pool lock: NO */
static inline void pool_set_dom_id(pool *p, int dom_id)
{
  return atomic_store_explicit(&p->free_list.domain_id, dom_id,
                               memory_order_relaxed);
}
#else
static inline void pool_set_dom_id(pool *p, int n) { (void)p; (void)n; }
#endif // OCAML_MULTICORE

static pool_rings * init_pool_rings(int dom_id);

/* requires domain lock: NO
   requires pool lock: NO */
static inline void acquire_pool_rings(int dom_id)
{
  boxroot_mutex_lock(&pools[dom_id]->mutex);
}

/* requires domain lock: NO
   requires pool lock: YES */
static inline void release_pool_rings(int dom_id)
{
  boxroot_mutex_unlock(&pools[dom_id]->mutex);
}

/* requires domain lock: NO
   requires pool lock: NO */
static inline int acquire_pool_rings_of_pool(pool *p)
{
  int dom_id = dom_id_of_pool(p);
  while (1) {
    DEBUGassert(pools[dom_id] != NULL);
    acquire_pool_rings(dom_id);
    int new_dom_id = dom_id_of_pool(p);
    if (dom_id == new_dom_id) return dom_id;
    /* Pool owner has changed before we could lock it. Try again. */
    release_pool_rings(dom_id);
    dom_id = new_dom_id;
  }
}

/* requires domain lock: NO
   requires pool lock: NO */
static pool_rings * alloc_pool_rings()
{
  pool_rings *ps = (pool_rings *)malloc(sizeof(pool_rings));
  if (ps == NULL) goto out_err;
  if (!boxroot_initialize_mutex(&ps->mutex)) goto out_err;
  return ps;
 out_err:
  free(ps);
  return NULL;
}

/* requires domain lock: YES
   requires pool lock: NO */
// TODO simplify: no return
static pool_rings * init_pool_rings(int dom_id)
{
  pool_rings *local = pools[dom_id];
  if (local == NULL) local = alloc_pool_rings();
  if (local == NULL) return NULL;
  local->old = NULL;
  local->young = NULL;
  local->current = NULL;
  local->free = NULL;
  boxroot_current_fl[dom_id] = &empty_fl;
  pools[dom_id] = local;
  return local;
}

static struct {
  stat_t minor_collections;
  stat_t major_collections;
  stat_t total_create_young;
  stat_t total_create_old;
  stat_t total_create_slow;
  stat_t total_delete_young;
  stat_t total_delete_old;
  stat_t total_delete_slow;
  stat_t total_modify;
  stat_t total_scanning_work_minor;
  stat_t total_scanning_work_major;
  stat_t total_minor_time;
  stat_t total_major_time;
  stat_t peak_minor_time;
  stat_t peak_major_time;
  stat_t total_alloced_pools;
  stat_t total_emptied_pools;
  stat_t total_freed_pools;
  stat_t live_pools; // number of tracked pools
  stat_t peak_pools; // max live pools at any time
  stat_t ring_operations; // Number of times p->next is mutated
  stat_t young_hit_gen; /* number of times a young value was encountered
                           during generic scanning (not minor collection) */
  stat_t young_hit_young; /* number of times a young value was encountered
                             during young scanning (minor collection) */
  stat_t get_pool_header; // number of times get_pool_header was called
  stat_t is_pool_member; // number of times is_pool_member was called
} stats;

/* }}} */

/* {{{ Tests in the hot path */

// hot path
/* requires domain lock: NO
   requires pool lock: NO */
static inline pool * get_pool_header(slot *s)
{
  if (DEBUG) incr(&stats.get_pool_header);
  return Get_pool_header(s);
}

// Return true iff v shares the same msbs as p and is not an
// immediate.
// hot path
/* requires domain lock: NO
   requires pool lock: NO */
static inline int is_pool_member(slot v, pool *p)
{
  if (DEBUG) incr(&stats.is_pool_member);
  return (uintptr_t)p == ((uintptr_t)v & ~((uintptr_t)POOL_SIZE - 2));
}

// hot path
static inline int is_empty_free_list(slot *v, pool *p)
{
  return (v == (slot *)p);
}

/* }}} */

/* {{{ Ring operations */

/* requires domain lock: NO
   requires pool lock: YES */
static inline void ring_link(pool *p, pool *q)
{
  p->next = q;
  q->prev = p;
  incr(&stats.ring_operations);
}

/* insert the ring [source] at the back of [*target]. */
/* requires domain lock: NO
   requires pool lock: YES */
static inline void ring_push_back(pool *source, pool **target)
{
  if (source == NULL) return;
  DEBUGassert(source->prev == source && source->next == source);
  DEBUGassert(source != *target);
  if (*target == NULL) {
    *target = source;
  } else {
    DEBUGassert((*target)->class == source->class);
    pool *target_last = (*target)->prev;
    pool *source_last = source->prev;
    ring_link(target_last, source);
    ring_link(source_last, *target);
  }
}

// remove the first element from [*target] and return it
/* requires domain lock: NO
   requires pool lock: YES */
static pool * ring_pop(pool **target)
{
  pool *front = *target;
  DEBUGassert(front != NULL);
  if (front->next == front) {
    *target = NULL;
  } else {
    *target = front->next;
    ring_link(front->prev, front->next);
  }
  ring_link(front, front);
  return front;
}

/* }}} */

/* {{{ Pool management */

/* the empty free-list for a pool p is denoted by a pointer to the
   pool itself (NULL could be a valid value for an element slot) */
static inline slot empty_free_list(pool *p) { return (slot)p; }

static inline int is_full_pool(pool *p)
{
  return is_empty_free_list(p->free_list.next, p);
}

/* requires domain lock: NO
   requires pool lock: NO */
static pool * get_empty_pool()
{
  long long live_pools = incr(&stats.live_pools);
  /* racy, but whatever */
  if (live_pools > stats.peak_pools) stats.peak_pools = live_pools;
  pool *p = boxroot_alloc_uninitialised_pool(POOL_SIZE);
  if (p == NULL) return NULL;
  incr(&stats.total_alloced_pools);
  ring_link(p, p);
  p->class = UNTRACKED;
  p->free_list.next = p->roots;
  p->free_list.alloc_count = 0;
  p->free_list.end = &p->roots[POOL_CAPACITY - 1];
  pool_set_dom_id(p, -1);
  p->delayed_fl.next = &p->delayed_fl; // Empty free list. TODO: simplify
  p->delayed_fl.alloc_count = 0;
  p->delayed_fl.end = NULL;
  /* We end the freelist with a dummy value which satisfies is_pool_member */
  p->roots[POOL_CAPACITY - 1] = empty_free_list(p);
  for (slot *s = p->roots + POOL_CAPACITY - 2; s >= p->roots; --s) {
    *s = (slot)(s + 1);
  }
  return p;
}

/* requires domain lock: YES
   requires pool lock: YES */
static void gc_pool(pool *p)
{
  if (0 == p->delayed_fl.alloc_count) return;
  if (is_full_pool(p)) p->free_list.end = p->delayed_fl.end;
  p->free_list.alloc_count += p->delayed_fl.alloc_count;
  p->delayed_fl.alloc_count = 0;
  void *list = p->free_list.next;
  p->free_list.next = p->delayed_fl.next;
  p->delayed_fl.next = &p->delayed_fl; // Empty free list
  *((slot *)p->delayed_fl.end) = list;
}

/* requires domain lock: NO
   requires pool lock: YES */
static void free_pool_ring(pool **ring)
{
  while (*ring != NULL) {
    pool *p = ring_pop(ring);
    boxroot_free_pool(p);
    incr(&stats.total_freed_pools);
  }
}

/* requires domain lock: NO
   requires pool lock: YES */
static void free_pool_rings(pool_rings *ps)
{
  free_pool_ring(&ps->old);
  free_pool_ring(&ps->young);
  free_pool_ring(&ps->current);
  free_pool_ring(&ps->free);
}

/* }}} */

/* {{{ Pool class management */

/* requires domain lock: YES
   requires pool lock: NO */
static inline int is_not_too_full(pool *p)
{
  return p->free_list.alloc_count <= (int)(DEALLOC_THRESHOLD / sizeof(slot));
}

/* requires domain lock: YES
   requires pool lock: NO */
static void set_current_pool(int dom_id, pool *p)
{
  DEBUGassert(pools[dom_id]->current == NULL);
  if (p != NULL) {
    pool_set_dom_id(p, dom_id);
    pools[dom_id]->current = p;
    p->class = YOUNG;
    /* This assumption is made inside boxroot_delete */
    DEBUGassert(&p->free_list == (boxroot_fl *)p);
    boxroot_current_fl[dom_id] = &p->free_list;
  } else {
    boxroot_current_fl[dom_id] = &empty_fl;
  }
}

static void reclassify_pool(pool **source, int dom_id, class cl);

/* Move not-too-full pools to the front; move empty pools to the free
   ring. */
/* requires domain lock: YES
   requires pool lock: NO */
static void try_demote_pool(pool *p)
{
  DEBUGassert(p->class != UNTRACKED);
  int dom_id = dom_id_of_pool(p);
  pool_rings *remote = pools[dom_id];
  if (p == remote->current || !is_not_too_full(p)) return;
  acquire_pool_rings(dom_id);
  class cl = (p->free_list.alloc_count == 0) ? UNTRACKED : p->class;
  /* If the pool is at the head of its ring, the new head must be
     recorded. */
  pool **source = (p == remote->old) ? &remote->old :
                  (p == remote->young) ? &remote->young : &p;
  reclassify_pool(source, dom_id, cl);
  release_pool_rings(dom_id);
}

/* requires domain lock: YES
   requires pool lock: YES */
static inline pool * pop_available(pool **target)
{
  /* When pools empty themselves enough, they are pushed to the front.
     When they fill up, they are pushed to the back. Thus, if the
     first one is full, then none of the next ones are empty
     enough. */
  if (*target == NULL || is_full_pool(*target)) return NULL;
  return ring_pop(target);
}

/* Find an available pool and set it as current. Return NULL if none
   was found and the allocation of a new one failed. */
/* requires domain lock: YES
   requires pool lock: YES */
static pool * find_available_pool(int dom_id)
{
  pool_rings *local = pools[dom_id];
  pool *p = pop_available(&local->young);
  if (p == NULL && local->old != NULL && is_not_too_full(local->old))
    p = pop_available(&local->old);
  if (p == NULL) p = pop_available(&local->free);
  if (p == NULL) p = get_empty_pool();
  DEBUGassert(local->current == NULL);
  set_current_pool(dom_id, p);
  return p;
}

static void validate_all_pools(int dom_id);

/* move the head of [source] to the appropriate ring in domain
   [dom_id] determined by [class]. Not-too-full pools are pushed to
   the front. */
/* requires domain lock: YES
   requires pool lock: YES */
static void reclassify_pool(pool **source, int dom_id, class cl)
{
  DEBUGassert(*source != NULL);
  pool_rings *local = pools[dom_id];
  pool *p = ring_pop(source);
  pool_set_dom_id(p, dom_id);
  pool **target = NULL;
  switch (cl) {
  case OLD: target = &local->old; break;
  case YOUNG: target = &local->young; break;
  case UNTRACKED:
    target = &local->free;
    incr(&stats.total_emptied_pools);
    decr(&stats.live_pools);
    break;
  }
  /* protected by domain lock */
  p->class = cl;
  ring_push_back(p, target);
  /* make p the new head of [*target] (rotate one step backwards) if
     it is not too full. */
  if (is_not_too_full(p)) *target = p;
}

/* requires domain lock: YES
   requires pool lock: YES */
static void promote_young_pools(int dom_id)
{
  pool_rings *local = pools[dom_id];
  // Promote full pools
  while (local->young != NULL) {
    reclassify_pool(&local->young, dom_id, OLD);
  }
  // There is no current pool to promote. Ensure that a domain that
  // does not use any boxroot between two minor collections does not
  // pay the cost of scanning any pool.
  DEBUGassert(local->current == NULL);
}

/* }}} */

/* {{{ Allocation, deallocation */

enum { NOT_SETUP, RUNNING, ERROR };

/* Thread-safety: see documented constraints on the use of
   boxroot_setup and boxroot_teardown. */
#if OCAML_MULTICORE
static atomic_int status = NOT_SETUP;
#else
static int status = NOT_SETUP;
#endif

static int setup();

// Set an available pool as current and allocate from it.
/* requires domain lock: YES
   requires pool lock: NO */
boxroot boxroot_create_slow(value init)
{
  incr(&stats.total_create_slow);
  if (Caml_state_opt == NULL) return NULL;
  // We might be here because boxroot is not setup.
  if (status != RUNNING && 0 == setup()) return NULL;
#if !OCAML_MULTICORE
  boxroot_check_thread_hooks();
#endif
  int dom_id = Domain_id;
  pool_rings *local = pools[dom_id];
  /* Initialize pool rings on this domain */
  if (local == NULL) local = init_pool_rings(dom_id);
  if (local == NULL) return NULL;
  acquire_pool_rings(dom_id);
  pool *p = local->current;
  if (p != NULL) {
    gc_pool(p);
    if (is_full_pool(p)) {
      p = NULL;
      reclassify_pool(&local->current, dom_id, YOUNG);
    }
  }
  if (p == NULL) p = find_available_pool(dom_id);
  release_pool_rings(dom_id);
  if (p == NULL) return NULL;
  DEBUGassert(!is_full_pool(p));
  return boxroot_create(init);
}

/* }}} */

/* {{{ Boxroot API implementation */

extern inline value boxroot_get(boxroot root);
extern inline value const * boxroot_get_ref(boxroot root);

void boxroot_create_debug(value init)
{
  DEBUGassert(Caml_state_opt != NULL);
  if (Is_block(init) && Is_young(init)) incr(&stats.total_create_young);
  else incr(&stats.total_create_old);
}

extern inline boxroot boxroot_create(value init);

/* Needed to avoid linking error with Rust */
extern inline int boxroot_free_slot(boxroot_fl *fl, boxroot root);

void boxroot_delete_debug(boxroot root)
{
  DEBUGassert(root != NULL);
  value v = boxroot_get(root);
  if (Is_block(v) && Is_young(v)) incr(&stats.total_delete_young);
  else incr(&stats.total_delete_old);
}

/* requires domain lock: NO
   requires pool lock: NO */
void boxroot_delete_slow(boxroot root)
{
  incr(&stats.total_delete_slow);
  /* recomputing these avoids spilling in boxroot_delete */
  pool *p = get_pool_header((slot)root);
  int dom_id = dom_id_of_pool(p);
  int local = !BOXROOT_FORCE_REMOTE && boxroot_domain_lock_held(dom_id);
  if (local) {
    /* deallocation already done, but we passed a deallocation
       threshold */
    try_demote_pool(p);
  } else {
    /* delayed deallocation */
    int remote_dom_id = acquire_pool_rings_of_pool(p);
    boxroot_free_slot(&p->delayed_fl, root);
    release_pool_rings(remote_dom_id);
  }
}

extern inline void boxroot_delete(boxroot root);

/* requires domain lock: YES
   requires pool lock: YES */
static void boxroot_reallocate(boxroot *root, value new_value)
{
  boxroot old = *root;
  boxroot new = boxroot_create(new_value);
  if (BOXROOT_LIKELY(new != NULL)) {
    *root = new;
    boxroot_delete(old);
  } else {
    // Better not fail in boxroot_modify. Expensive but fail-safe:
    // demote its pool into the young pools.
    pool *p = get_pool_header((slot)old);
    int dom_id = acquire_pool_rings_of_pool(p);
    DEBUGassert(p->class == OLD);
    pool_rings *remote = pools[dom_id];
    pool **source = (p == remote->old) ? &remote->old : &p;
    reclassify_pool(source, dom_id, YOUNG);
    **((value **)root) = new_value;
    release_pool_rings(dom_id);
  }
}

// hot path
/* TODO: fewer locks? */
/* requires domain lock: YES
   requires pool lock: NO */
void boxroot_modify(boxroot *root, value new_value)
{
  slot *s = (slot *)*root;
  pool *p = get_pool_header(s);
  DEBUGassert(s);
  if (DEBUG) incr(&stats.total_modify);
  if (BOXROOT_LIKELY(p->class == YOUNG
                     || !Is_block(new_value)
                     || !Is_young(new_value))) {
    /* Race with scanning */
    int dom_id = acquire_pool_rings_of_pool(p);
    *(value *)s = new_value;
    release_pool_rings(dom_id);
  } else {
    // We need to reallocate, but this reallocation happens at most once
    // between two minor collections.
    boxroot_reallocate(root, new_value);
  }
}

/* }}} */

/* {{{ Scanning */

/* requires domain lock: YES
   requires pool lock: YES */
static void validate_pool(pool *pl)
{
  if (pl->free_list.next == NULL) {
    // an unintialised pool
    assert(pl->class == UNTRACKED);
    return;
  }
  // check freelist structure and length
  slot *curr = pl->free_list.next;
  int pos = 0;
  for (; !is_empty_free_list(curr, pl); curr = (slot*)*curr, pos++)
  {
    assert(pos < POOL_CAPACITY);
    assert(curr >= pl->roots && curr < pl->roots + POOL_CAPACITY);
  }
  assert(pos == POOL_CAPACITY - pl->free_list.alloc_count);
  // check count of allocated elements
  int count = 0;
  for(int i = 0; i < POOL_CAPACITY; i++) {
    slot s = pl->roots[i];
    --stats.is_pool_member;
    if (!is_pool_member(s, pl)) {
      value v = (value)s;
      if (pl->class != YOUNG && Is_block(v)) assert(!Is_young(v));
      ++count;
    }
  }
  assert(count == pl->free_list.alloc_count);
}

/* requires domain lock: YES
   requires pool lock: YES */
static void validate_ring(pool **ring, int dom_id, class cl)
{
  pool *start_pool = *ring;
  if (start_pool == NULL) return;
  pool *p = start_pool;
  do {
    assert(dom_id_of_pool(p) == dom_id);
    assert(p->class == cl);
    validate_pool(p);
    assert(p->next != NULL);
    assert(p->next->prev == p);
    assert(p->prev != NULL);
    assert(p->prev->next == p);
    p = p->next;
  } while (p != start_pool);
}

/* requires domain lock: YES
   requires pool lock: YES */
static void validate_all_pools(int dom_id)
{
  pool_rings *local = pools[dom_id];
  validate_ring(&local->old, dom_id, OLD);
  validate_ring(&local->young, dom_id, YOUNG);
  validate_ring(&local->current, dom_id, YOUNG);
  validate_ring(&local->free, dom_id, UNTRACKED);
}

static void gc_pool_rings(int dom_id);

/* requires domain lock: YES
   requires pool lock: NO */
static void orphan_pools(int dom_id)
{
  pool_rings *local = pools[dom_id];
  if (local == NULL) return;
  acquire_pool_rings(dom_id);
  gc_pool_rings(dom_id);
  acquire_pool_rings(Orphaned_id);
  pool_rings *orphaned = pools[Orphaned_id];
  /* Move active pools to the orphaned pools. TODO: NUMA awareness? */
  ring_push_back(local->old, &orphaned->old);
  ring_push_back(local->young, &orphaned->young);
  ring_push_back(local->current, &orphaned->young);
  release_pool_rings(Orphaned_id);
  /* Free the rest */
  free_pool_ring(&local->free);
  /* Reset local pools for later domains spawning with the same id */
  init_pool_rings(dom_id);
  release_pool_rings(dom_id);
}

/* requires domain lock: NO
   requires pool lock: YES (dom_id) */
static void adopt_orphaned_pools(int dom_id)
{
  acquire_pool_rings(Orphaned_id);
  pool_rings *orphaned = pools[Orphaned_id];
  while (orphaned->old != NULL)
    reclassify_pool(&orphaned->old, dom_id, OLD);
  while (orphaned->young != NULL)
    reclassify_pool(&orphaned->young, dom_id, YOUNG);
  release_pool_rings(Orphaned_id);
}

static void gc_and_reclassify_pool(pool **source, int dom_id)
{
  pool *p = *source;
  gc_pool(p);
  if (p->free_list.alloc_count == 0) reclassify_pool(source, dom_id, UNTRACKED);
  else if (is_not_too_full(p)) reclassify_pool(source, dom_id, p->class);
}

/* empty the delayed free lists in a ring and move the pools
   accordingly */
/* requires domain lock: YES
   requires pool lock: YES */
static void gc_ring(pool **ring, int dom_id)
{
  if (!BOXROOT_MULTITHREAD) return;
  pool *p = *ring;
  if (p == NULL) return;
  /* This is a bit convoluted because we do in-place GC-ing of the
     ring: we never move pools that did not need to be GCd. We
     distinguish two cases: if we are at the head of the ring or
     inside the tail. */
  /* GC the head of the ring while we are still at the head. */
  while (p == *ring) {
    pool *next = p->next;
    if (p->delayed_fl.alloc_count != 0)
      gc_and_reclassify_pool(ring, dom_id);
    if (p == next)
      /* There was only one pool left in the ring */
      return;
    p = next;
  }
  /* Now p != *ring, and things become easier */
  do {
    pool *next = p->next;
    if (p->delayed_fl.alloc_count != 0)
      gc_and_reclassify_pool(&p, dom_id);
    p = next;
  } while (p != *ring);
}

/* empty the delayed free lists in the chosen pool rings and
   move the pools accordingly */
/* requires domain lock: YES
   requires pool lock: YES */
static void gc_pool_rings(int dom_id)
{
  pool_rings *local = pools[dom_id];
  // Heuristic: if a young pool has just been allocated, it is better
  // if it is the first one to be considered the next time a young
  // boxroot allocation takes place. (First it ends up last, so that
  // it ends up to the front after pool promotion.)
  if (local->current != NULL) {
    reclassify_pool(&local->current, dom_id, YOUNG);
    set_current_pool(dom_id, NULL);
  }
  gc_ring(&local->young, dom_id);
  gc_ring(&local->old, dom_id);
}

// returns the amount of work done
/* requires domain lock: YES
   requires pool lock: YES */
static int scan_pool_gen(scanning_action action, void *data, pool *pl)
{
  int allocs_to_find = pl->free_list.alloc_count;
  int young_hit = 0;
  slot *current = pl->roots;
  while (allocs_to_find) {
    // hot path
    slot s = *current;
    if (!is_pool_member(s, pl)) {
      --allocs_to_find;
      value v = (value)s;
      if (DEBUG && Is_block(v) && Is_young(v)) ++young_hit;
      CALL_GC_ACTION(action, data, v, (value *)current);
    }
    ++current;
  }
  stats.young_hit_gen += young_hit;
  return current - pl->roots;
}

/* Specialised version of [scan_pool_gen] when [only_young].

   Benchmark results for minor scanning:
   20% faster for young hits=95%
   20% faster for young hits=50% (random)
   90% faster for young_hit=10% (random)
   280% faster for young hits=0%
*/
/* requires domain lock: YES
   requires pool lock: YES */
static int scan_pool_young(scanning_action action, void *data, pool *pl)
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
  slot *start = pl->roots;
  slot *end = start + POOL_CAPACITY;
  int young_hit = 0;
  slot *i;
  for (i = start; i < end; i++) {
    slot s = *i;
    value v = (value)s;
    /* Optimise for branch prediction: if v falls within the young
       range, then it is likely that it is a block */
    if ((uintnat)v - young_start <= young_range
        && BOXROOT_LIKELY(Is_block(v))) {
      ++young_hit;
      CALL_GC_ACTION(action, data, v, (value *)i);
    }
  }
  stats.young_hit_young += young_hit;
  return i - start;
}

/* requires domain lock: YES
   requires pool lock: YES */
static int scan_pool(scanning_action action, int only_young, void *data,
                     pool *pl)
{
  if (only_young)
    return scan_pool_young(action, data, pl);
  else
    return scan_pool_gen(action, data, pl);
}

/* requires domain lock: YES
   requires pool lock: YES */
static int scan_ring(scanning_action action, int only_young,
                     void *data, pool **ring)
{
  int work = 0;
  pool *start_pool = *ring;
  if (start_pool == NULL) return 0;
  pool *p = start_pool;
  do {
    work += scan_pool(action, only_young, data, p);
    p = p->next;
  } while (p != start_pool);
  return work;
}

/* requires domain lock: YES
   requires pool lock: YES */
static int scan_pools(scanning_action action, int only_young,
                      void *data, int dom_id)
{
  pool_rings *local = pools[dom_id];
  int work = scan_ring(action, only_young, data, &local->young);
  if (!only_young) work += scan_ring(action, 0, data, &local->old);
  return work;
}

/* requires domain lock: YES
   requires pool lock: YES */
static void scan_roots(scanning_action action, int only_young,
                       void *data, int dom_id)
{
  if (DEBUG) validate_all_pools(dom_id);
  /* First perform all the delayed deallocations. This also moves the
     current pool to the young pools. */
  gc_pool_rings(dom_id);
  /* The first domain arriving there will take ownership of the pools
     of terminated domains. */
  adopt_orphaned_pools(dom_id);
  int work = scan_pools(action, only_young, data, dom_id);
  if (boxroot_in_minor_collection()) {
    promote_young_pools(dom_id);
  } else {
    free_pool_ring(&pools[dom_id]->free);
  }
  if (only_young) stats.total_scanning_work_minor += work;
  else stats.total_scanning_work_major += work;
  if (DEBUG) validate_all_pools(dom_id);
}

/* }}} */

/* {{{ Statistics */

static long long time_counter(void)
{
#if defined(POSIX_CLOCK)
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * (long long)1000000000 + (long long)t.tv_nsec;
#else
  return 0;
#endif
}

// unit: 1=KiB, 2=MiB
static long long kib_of_pools(long long count, int unit)
{
  int log_per_pool = POOL_LOG_SIZE - unit * 10;
  if (log_per_pool >= 0) return count << log_per_pool;
  else return count >> -log_per_pool;
}

static double average(long long total, long long units)
{
  // round to nearest
  return ((double)total) / (double)units;
}

void boxroot_print_stats()
{
  printf("minor collections: %'lld\n"
         "major collections (and others): %'lld\n",
         stats.minor_collections,
         stats.major_collections);

  if (stats.total_alloced_pools == 0) return;

  printf("POOL_LOG_SIZE: %d (%'lld KiB, %'d roots/pool)\n"
         "DEBUG: %d\n"
         "OCAML_MULTICORE: %d\n"
         "BOXROOT_MULTITHREAD: %d\n"
         "WITH_EXPECT: 1\n",
         (int)POOL_LOG_SIZE, kib_of_pools(1, 1), (int)POOL_CAPACITY,
         (int)DEBUG, (int)OCAML_MULTICORE, (int)BOXROOT_MULTITHREAD);

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

  double scanning_work_minor =
    average(stats.total_scanning_work_minor, stats.minor_collections);
  double scanning_work_major =
    average(stats.total_scanning_work_major, stats.major_collections);
  long long total_scanning_work =
    stats.total_scanning_work_minor + stats.total_scanning_work_major;
#if DEBUG
  double young_hits_gen_pct =
    average(stats.young_hit_gen * 100, stats.total_scanning_work_major);
#endif
  double young_hits_young_pct =
    average(stats.young_hit_young * 100, stats.total_scanning_work_minor);

  printf("work per minor: %'.0f\n"
         "work per major: %'.0f\n"
         "total scanning work: %'lld (%'lld minor, %'lld major)\n"
#if DEBUG
         "young hits (non-minor collection): %.2f%%\n"
#endif
         "young hits (minor collection): %.2f%%\n",
         scanning_work_minor,
         scanning_work_major,
         total_scanning_work, stats.total_scanning_work_minor, stats.total_scanning_work_major,
#if DEBUG
         young_hits_gen_pct,
#endif
         young_hits_young_pct);

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

  double ring_operations_per_pool =
    average(stats.ring_operations, stats.total_alloced_pools);

  printf("total boxroot_create_slow: %'lld\n"
         "total boxroot_delete_slow: %'lld\n"
         "total ring operations: %'lld\n"
         "ring operations per pool: %.2f\n",
         stats.total_create_slow,
         stats.total_delete_slow,
         stats.ring_operations,
         ring_operations_per_pool);

#if DEBUG
  long long total_create = stats.total_create_young + stats.total_create_old;
  long long total_delete = stats.total_delete_young + stats.total_delete_old;
  double create_young_pct =
    average(stats.total_create_young * 100, total_create);
  double delete_young_pct =
    average(stats.total_delete_young * 100, total_delete);

  printf("total created: %'lld (%.2f%% young)\n"
         "total deleted: %'lld (%.2f%% young)\n"
         "total modified: %'lld\n",
         total_create, create_young_pct,
         total_delete, delete_young_pct,
         stats.total_modify);

  printf("get_pool_header: %'lld\n"
         "is_pool_member: %'lld\n",
         stats.get_pool_header,
         stats.is_pool_member);
#endif
}

/* }}} */

/* {{{ Hook setup */

/* requires domain lock: YES
   requires pool lock: NO */
static void scanning_callback(scanning_action action, int only_young,
                              void *data)
{
  if (status != RUNNING) return;
  int in_minor_collection = boxroot_in_minor_collection();
  if (in_minor_collection) incr(&stats.minor_collections);
  else incr(&stats.major_collections);
  int dom_id = Domain_id;
  if (pools[dom_id] == NULL) return; /* synchronised by domain lock */
  acquire_pool_rings(dom_id);
#if !OCAML_MULTICORE
  boxroot_check_thread_hooks();
#endif
  long long start = time_counter();
  scan_roots(action, only_young, data, dom_id);
  long long duration = time_counter() - start;
  stat_t *total = in_minor_collection ? &stats.total_minor_time : &stats.total_major_time;
  stat_t *peak = in_minor_collection ? &stats.peak_minor_time : &stats.peak_major_time;
  *total += duration;
  if (duration > *peak) *peak = duration; // racy, but whatever
  release_pool_rings(dom_id);
}

/* Handle orphaning of domain-local pools */
/* requires domain lock: YES
   requires pool lock: NO */
static void domain_termination_callback()
{
  DEBUGassert(OCAML_MULTICORE == 1);
  int dom_id = Domain_id;
  orphan_pools(dom_id);
}

/* Used for initialization/teardown */
static mutex_t init_mutex = BOXROOT_MUTEX_INITIALIZER;

/* requires domain lock: YES
   requires pool lock: NO */
static int setup()
{
  boxroot_mutex_lock(&init_mutex);
  if (status == RUNNING) goto out;
  if (status == ERROR) goto out_err;
  boxroot_setup_hooks(&scanning_callback, &domain_termination_callback);
  /* Domain 0 can be accessed without going through acquire_pool_rings
     on OCaml 4 without mutex, so we need to initialize it right away. */
  if (NULL == init_pool_rings(Orphaned_id)) goto out_err;
  // we are done
  status = RUNNING;
  // fall through
 out:
  boxroot_mutex_unlock(&init_mutex);
  return 1;
 out_err:
  status = ERROR;
  boxroot_mutex_unlock(&init_mutex);
  return 0;
}

/* obsolete */
int boxroot_setup() { return 1; }

/* requires domain lock: NO
   requires pool lock: NO

   We are sole owner of the pools at this point, no need for
   locking. */
void boxroot_teardown()
{
  boxroot_mutex_lock(&init_mutex);
  if (status != RUNNING) goto out;
  status = ERROR;
  for (int i = 0; i < Num_domains; i++) {
    pool_rings *ps = pools[i];
    if (ps == NULL) continue;
    free_pool_rings(ps);
    free(ps);
  }
  // fall through
 out:
  boxroot_mutex_unlock(&init_mutex);
}

/* }}} */

/* {{{ */
/* }}} */
