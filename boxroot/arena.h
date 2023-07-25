/* SPDX-License-Identifier: GPL-3.0 */
#ifndef BXR_ARENA_H
#define BXR_ARENA_H

/* Reap implementation (experimental) */

#include <caml/memory.h>
#include "platform.h"

typedef struct local_private* local_ref;

static inline value local_get(local_ref r) { return *(value *)r; }
static inline value * local_get_ref(local_ref r) { return (value *)r; }

typedef struct arena arena;

/* Assumes ownership of domain lock.
  Usage:

  value ocaml_c_wrapper()
  {
    arena a;
    init_arena(&a);
    […alloc/delete_local_ref…]
    drop_arena(&a);
    return res;
  }

  Caution about mixing arenas and CAMLparam: if you use CAMLparam,
  then alloc/delete_local_ref is unavailable until the corresponding
  CAMLreturn. You can call init_arena/drop_arena between
  CAMLparam/CAMLreturn if the calls are well-parenthezised.
*/
static inline void init_arena(arena *a);
static inline void drop_arena(arena *a);

static inline local_ref alloc_local_ref(value v);
static inline void delete_local_ref(local_ref l);


/* Private implementation: */

typedef struct caml__roots_block arena_data;

/* We essentially encode inside struct caml__roots_block:

typedef struct {
  value *pool;
  intnat size;
  value *free_list;
} arena_data;

*/

#define START_ITEMS 16 /* Must be a power of two */

typedef struct arena {
  arena_data data;
  value pool[START_ITEMS];
} arena;

#define ARENA_POOL_SIZE(ad) (*(intnat *)&(ad).tables[1])
#define ARENA_NEXT_INDEX(ad) ((ad).nitems)
#define ARENA_FREE_LIST(ad) ((ad).tables[3])

static inline void bxr_init_arena_with_size(arena *a, intnat size)
{
  arena_data *ad = &a->data;
  ad->next = Caml_state->local_roots;
  ad->ntables = 1;
  ad->tables[0] = a->pool;
  ARENA_POOL_SIZE(*ad) = size;
  ARENA_NEXT_INDEX(*ad) = 0;
  Caml_state->local_roots = ad;
  ARENA_FREE_LIST(*ad) = NULL;
}

/* Ownership of domain lock can be checked statically */
static inline void init_arena(arena *a)
{
  bxr_init_arena_with_size(a, START_ITEMS);
}

#define BXR_heuristic_assert_arena(ad)                                  \
  /* ntables is 1 and the pool size is a power of two*/                 \
  (CAMLassert((ad) != NULL &&                                           \
              (ad)->ntables == 1                                        \
              && (ARENA_POOL_SIZE(ad) & (ARENA_POOL_SIZE(ad) - 1)) == 0))

static inline arena_data * get_arena_data(void)
{
  arena_data *ad = Caml_state->local_roots;
  BXR_heuristic_assert_arena(*ad);
  return ad;
}

/* Ownership of domain lock can be checked statically */
static inline void drop_arena(arena *initial_arena)
{
  arena_data *ad = get_arena_data();
  while (ad != &initial_arena->data) {
    arena *current = (arena *)ad;
    ad = ad->next;
    free(current);
    BXR_heuristic_assert_arena(*ad);
  }
  Caml_state->local_roots = initial_arena->data.next;
}

#define VAL_OF_PTR(p) ((value)p | (value)1)
#define PTR_OF_VALUE(p) ((void *)(v & ~(value)1))

value * bxr_arena_alloc_slow();

static inline local_ref alloc_local_ref(value v)
{
//  if (BXR_UNLIKELY(!bxr_domain_lock_held())) return (local_ref)NULL;
  arena_data *ad = get_arena_data();
  value *res;
  if (ARENA_FREE_LIST(*ad) != NULL) {
    res = ARENA_FREE_LIST(*ad);
    ARENA_FREE_LIST(*ad) = PTR_OF_VALUE(*res);
  } else {
    intnat next_index = ARENA_NEXT_INDEX(*ad);
    if (BXR_LIKELY(next_index != ARENA_POOL_SIZE(*ad))) {
      ARENA_NEXT_INDEX(*ad)++;
      arena *a = (arena *)ad;
      res = &a->pool[next_index];
    } else {
      res = bxr_arena_alloc_slow();
    }
  }
  *res = v;
  return (local_ref)res;
}

static inline void delete_local_ref(local_ref l)
{
  /* Can't do much here, provide apples-to-apples comparison for perf */
//  if (BXR_UNLIKELY(!bxr_domain_lock_held())) return;
  arena_data *ad = get_arena_data();
  value *new_free_list = (value *)l;
  *new_free_list = VAL_OF_PTR(ARENA_FREE_LIST(*ad));
  ARENA_FREE_LIST(*ad) = new_free_list;
}

#endif // BXR_ARENA_H
