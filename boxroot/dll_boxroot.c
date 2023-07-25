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

#include "dll_boxroot.h"
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

// values are stored in "ring elements"
// rings: cyclic doubly-linked lists
struct elem {
  value slot;
  struct elem *prev;
  struct elem *next;
};

typedef struct elem *ring;

/* }}} */

/* {{{ Globals */

/* Global rings. */
static struct {
  /* list of elements with young values */
  ring young;

  /* list of elements with old values */
  ring old;

  /* list of empty elements */
  ring free;
} rings;

static void validate_all_rings();

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
  long long is_young; // count 'is_young_block' checks
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
#define FOREACH_ELEM_IN_RING(elem, ring, action) do { \
    if (ring == NULL) break;                          \
    struct elem *elem = ring;                         \
    do {                                              \
      action;                                         \
      elem = elem->next;                              \
    } while (elem != ring);                           \
  } while (0)

inline static void ring_link(ring p, ring q)
{
  p->next = q;
  q->prev = p;
}

// insert the ring [source] at the back of [*target].
static void ring_push_back(ring source, ring *target)
{
  if (source == NULL) return;
  if (*target == NULL) {
    *target = source;
  } else {
    struct elem *target_last = (*target)->prev;
    struct elem *source_last = source->prev;
    ring_link(target_last, source);
    ring_link(source_last, *target);
  }
}

// remove the first element from [*target] and return it
static ring ring_pop(ring *target)
{
  struct elem *front = *target;
  assert(front);
  if (front->next == front) {
    *target = NULL;
  } else {
    *target = front->next;
    ring_link(front->prev, front->next);
  }
  ring_link(front, front);
  return front;
}

static ring ring_pop_elem(ring elem)
{
  ring prev = ring_pop(&elem);
  if (rings.young == prev) rings.young = elem;
  if (rings.old == prev) rings.old = elem;
  return prev;
}

void free_ring(ring r) {
  if (r == NULL) return;
  struct elem *cur = r;
  do {
    struct elem *next = cur->next;
    free(cur);
    cur = next;
  } while (cur != r);
}
/* }}} */

/* {{{ Ring of free elements */

ring create_elem() {
  if (rings.free != NULL) {
    return ring_pop(&rings.free);
  }
  struct elem *elem = malloc(sizeof (struct elem));
  if (elem == NULL) {
    return NULL;
  }
  ring_link(elem, elem);
  return elem;
}

void delete_elem(ring elem) {
  elem->slot = (value)NULL;
  ring_push_back(elem, &rings.free);
}

/* }}} */

/* {{{ Boxroot API implementation */

static inline int is_young_block(value v)
{
  if (BOXROOT_DEBUG) ++stats.is_young;
  return Is_block(v) && Is_young(v);
}

static inline void track_elem(ring elem) {
  ring *dst = is_young_block(elem->slot) ? &rings.young : &rings.old;
  ring_push_back(elem, dst);
}

dll_boxroot dll_boxroot_create(value init)
{
  if (BOXROOT_DEBUG) ++stats.total_create;
  ring root = create_elem();
  root->slot = init;
  track_elem(root);
  return (dll_boxroot)root;
}

value dll_boxroot_get(dll_boxroot root)
{
  return ((ring)root)->slot;
}

value const * dll_boxroot_get_ref(dll_boxroot root)
{
  return &(((ring)root)->slot);
}

void dll_boxroot_delete(dll_boxroot root)
{
  struct elem *elem = (ring)root;
  delete_elem(ring_pop_elem(elem));
}

void dll_boxroot_modify(dll_boxroot *root, value new_value)
{
  ring elem = (ring)*root;
  value old_value = elem->slot;
  if (is_young_block(old_value) || !is_young_block(new_value)) {
    elem->slot = new_value;
  } else {
    ring new_elem = ring_pop_elem(elem);
    new_elem->slot = new_value;
    track_elem(new_elem);
    *root = (dll_boxroot)new_elem;
  }
}

/* }}} */

/* {{{ Scanning */

static void validate_young_ring() {
  // nothing to check: the young ring
  // may contain both new and old values
  // (including NULL, if roots are used from C)
}

static void validate_old_ring() {
  FOREACH_ELEM_IN_RING(elem, rings.old, {
    assert(!is_young_block(elem->slot));
  });
}

static void validate_free_ring() {
  FOREACH_ELEM_IN_RING(elem, rings.free, {
    assert(elem->slot == (value)NULL);
  });
}

static void validate_all_rings() {
  struct stats stats_before = stats;
  validate_young_ring();
  validate_old_ring();
  validate_free_ring();
  stats = stats_before;
}

// returns the amount of work done
static int scan_ring(scanning_action action, void *data, ring r)
{
  if (r == NULL) return 0;
  int work = 0;
  FOREACH_ELEM_IN_RING(elem, r, {
    CALL_GC_ACTION(action, data, elem->slot, &elem->slot);
    work++;
  });
  return work;
}

static void scan_roots(scanning_action action, void *data)
{
  int work = 0;
  if (BOXROOT_DEBUG) validate_all_rings();
  work += scan_ring(action, data, rings.young);
  if (bxr_in_minor_collection()) {
    ring_push_back(rings.young, &rings.old);
    rings.young = NULL;
    stats.total_scanning_work_minor += work;
  } else {
    work += scan_ring(action, data, rings.old);
    free_ring(rings.free);
    rings.free = NULL;
    stats.total_scanning_work_major += work;
  }
  if (BOXROOT_DEBUG) validate_all_rings();
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

static int average(long long total_work, int nb_collections)
{
  if (nb_collections <= 0) return -1;
  // round to nearest
  return (total_work + (nb_collections / 2)) / nb_collections;
}

void dll_boxroot_print_stats()
{
  printf("minor collections: %d\n"
         "major collections (and others): %d\n",
         stats.minor_collections,
         stats.major_collections);

#if BOXROOT_DEBUG
  printf("total created: %'d\n"
         "total deleted: %'d\n"
         "total modified: %'d\n",
         stats.total_create,
         stats.total_delete,
         stats.total_modify);

  printf("is_young_block: %'lld\n",
         stats.is_young);
#endif
  int scanning_work_minor = average(stats.total_scanning_work_minor, stats.minor_collections);
  int scanning_work_major = average(stats.total_scanning_work_major, stats.major_collections);
  long long total_scanning_work = stats.total_scanning_work_minor + stats.total_scanning_work_major;

  int64_t time_per_minor = stats.minor_collections ?
    stats.total_minor_time / stats.minor_collections : 0;
  int64_t time_per_major = stats.major_collections ?
    stats.total_major_time / stats.major_collections : 0;

  printf("work per minor: %'d\n"
         "work per major: %'d\n"
         "total scanning work: %'lld (%'lld minor, %'lld major)\n",
         scanning_work_minor,
         scanning_work_major,
         total_scanning_work, stats.total_scanning_work_minor, stats.total_scanning_work_major);

#if defined(POSIX_CLOCK)
  printf("average time per minor: %'lldns\n"
         "average time per major: %'lldns\n"
         "peak time per minor: %'lldns\n"
         "peak time per major: %'lldns\n",
         (long long)time_per_minor,
         (long long)time_per_major,
         (long long)stats.peak_minor_time,
         (long long)stats.peak_major_time);
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
int dll_boxroot_setup()
{
  if (setup) return 0;
  // initialise globals
  struct stats empty_stats = {0};
  stats = empty_stats;
  rings.young = NULL;
  rings.old = NULL;
  rings.free = NULL;
  bxr_setup_hooks(&scanning_callback, NULL);
  // we are done
  setup = 1;
  if (BOXROOT_DEBUG) validate_all_rings();
  return 1;
}

void dll_boxroot_teardown()
{
  if (!setup) return;
  setup = 0;
  free_ring(rings.young);
  rings.young = NULL;
  free_ring(rings.old);
  rings.old = NULL;
  free_ring(rings.free);
  rings.free = NULL;
}

/* }}} */
