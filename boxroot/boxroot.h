/* SPDX-License-Identifier: MIT */
#ifndef BOXROOT_H
#define BOXROOT_H

#include <stdbool.h>
#include "ocaml_hooks.h"
#include "platform.h"

/* `boxroot`s follow an ownership discipline. */
typedef struct boxroot_private* boxroot;

/* `boxroot_create(v)` allocates a new boxroot initialised to the
   value `v`. This value will be considered as a root by the OCaml GC
   as long as the boxroot lives or until it is modified. The OCaml
   domain lock must be held before calling `boxroot_create`.

   A return value of `NULL` indicates a failure of allocation or
   initialization of Boxroot (see `boxroot_status`). */
inline boxroot boxroot_create(value);

/* `boxroot_get(r)` returns the contained value, subject to the usual
   discipline for non-rooted values. `boxroot_get_ref(r)` returns a
   pointer to a memory cell containing the value kept alive by `r`,
   that gets updated whenever its block is moved by the OCaml GC. The
   pointer becomes invalid after any call to `boxroot_delete(r)` or
   `boxroot_modify(&r,v)`. The argument must be non-null.

   The OCaml domain lock must be held before calling `boxroot_get` or
   before deferencing the result of `boxroot_get_ref`.
*/
inline value boxroot_get(boxroot r) { return *(value *)r; }
inline value const * boxroot_get_ref(boxroot r) { return (value *)r; }

/* `boxroot_delete(r)` deallocates the boxroot `r`. The value is no
   longer considered as a root by the OCaml GC. The argument must be
   non-null. (One does not need to hold the OCaml domain lock before
   calling `boxroot_delete`.)*/
inline void boxroot_delete(boxroot);

/* `boxroot_modify(&r,v)` changes the value kept alive by the boxroot
   `r` to `v`. It is essentially equivalent to the following:
   ```
   boxroot_delete(r);
   r = boxroot_create(v);
   ```
   However, `boxroot_modify` is more efficient: it avoids reallocating
   the boxroot if possible. The reallocation, if needed, occurs at
   most once between two minor collections.

   The OCaml domain lock must be held before calling `boxroot_modify`.

   A return value of `false` indicates that the modification could not
   take place due to a failure of reallocation (see `boxroot_status`).
*/
inline bool boxroot_modify(boxroot *, value);

/* `boxroot_teardown()` releases all the resources of Boxroot. None of
   the function above must be called after this. `boxroot_teardown`
   can only be called after OCaml shuts down. */
void boxroot_teardown();

/* For API authors, `boxroot_status()` shows the cause of an
   allocation failure:

   - Permanent failures:
       - `BOXROOT_TORE_DOWN`: `boxroot_teardown` has been called.
       - `BOXROOT_INVALID`: in OCaml 4, initializing the thread
         machinery after Boxroot has been intialized overwrites the
         hooks we use. Threads should be initialized before Boxroot.
         Either:
           - From OCaml, make sure the `Thread` module is already
             initialized when allocating the first boxroot.
           - From C, if calling `caml_startup` by hand, then you can
             call `caml_thread_initialize` immediately afterwards (but
             only with OCaml 4). With OCaml 5 there is nothing to do.

   - Transient failures (`BOXROOT_RUNNING`), check `errno`:
       - `errno == EPERM`: you tried calling `boxroot_create` or
         `boxroot_modify` without holding the domain lock.
       - `errno == ENOMEM`: allocation failure of the backing store. */
enum {
  BOXROOT_NOT_SETUP,
  BOXROOT_RUNNING,
  BOXROOT_TORE_DOWN,
  BOXROOT_INVALID
};
int boxroot_status();

/* Show some statistics on the standard output. */
void boxroot_print_stats();

/* Obsolete, does nothing. */
bool boxroot_setup();


/* ================================================================= */


/* Private implementation */

/* TODO: name spacing */
typedef /* _Atomic */ union slot *slot_ref;

typedef union slot {
  slot_ref as_slot_ref;
  value as_value;
} slot;

typedef struct boxroot_fl {
  slot_ref next;
  /* if non-empty, points to last cell */
  slot_ref end;
  /* length of the list */
  int alloc_count;
  int domain_id;
  /* kept in sync with its location in the pool rings. */
  int class;
} boxroot_fl;

#define CLASS_YOUNG 0

extern _Thread_local ptrdiff_t cached_domain_id;
extern boxroot_fl *boxroot_current_fl[Num_domains + 1];

void boxroot_create_debug(value v);
boxroot boxroot_create_slow(value v);

/* Test the overheads of multithreading (systhreads and multicore).
   Purely for experimental purposes. Otherwise should always be true. */
#define BOXROOT_MULTITHREAD true
/* Make every deallocation a remote deallocation. For testing purposes
   only. Otherwise should always be 0. */
#define BOXROOT_FORCE_REMOTE false

inline boxroot boxroot_create(value init)
{
#if defined(BOXROOT_DEBUG) && (BOXROOT_DEBUG == 1)
  boxroot_create_debug(init);
#endif
  /* Find current freelist. Synchronized by domain lock. */
  ptrdiff_t dom_id =
    (OCAML_MULTICORE && BOXROOT_MULTITHREAD) ? cached_domain_id : 0;
  boxroot_fl *fl = boxroot_current_fl[dom_id + 1];
  slot_ref new_root = fl->next;
  if (BOXROOT_UNLIKELY(!boxroot_domain_lock_held())
      || BOXROOT_UNLIKELY(new_root == (slot_ref)fl))
    return boxroot_create_slow(init);
  fl->next = new_root->as_slot_ref;
  fl->alloc_count++;
  new_root->as_value = init;
  return (boxroot)new_root;
}

/* Log of the size of the pools (12 = 4KB, an OS page).
   Recommended: 14. */
#define POOL_LOG_SIZE 14
#define POOL_SIZE ((size_t)1 << POOL_LOG_SIZE)
/* Every DEALLOC_THRESHOLD deallocations, make a pool available for
   allocation or demotion into a young pool, or reclassify it as an
   empty pool if empty. Change this with benchmarks in hand. Must be a
   power of 2. */
#define DEALLOC_THRESHOLD ((int)POOL_SIZE / 2)

#define Get_pool_header(s)                                        \
  ((boxroot_fl *)((uintptr_t)(s) & ~((uintptr_t)POOL_SIZE - 1)))

inline bool boxroot_free_slot(boxroot_fl *fl, boxroot root)
{
  /* We have the lock of the domain that owns the pool. */
  slot_ref s = (slot_ref)root;
  slot_ref next = fl->next;
  s->as_slot_ref = next;
  if (BOXROOT_MULTITHREAD && BOXROOT_UNLIKELY(next == (slot_ref)fl)) fl->end = s;
  fl->next = s;
  int alloc_count = --fl->alloc_count;
  return (alloc_count & (DEALLOC_THRESHOLD - 1)) == 0;
}

void boxroot_delete_debug(boxroot root);
void boxroot_delete_slow(boxroot_fl *fl, boxroot root, bool remote);

inline void boxroot_delete(boxroot root)
{
#if defined(BOXROOT_DEBUG) && (BOXROOT_DEBUG == 1)
  boxroot_delete_debug(root);
#endif
  boxroot_fl *fl = Get_pool_header(root);
  bool remote_dom_id =
    OCAML_MULTICORE ? fl->domain_id != cached_domain_id : false;
  bool remote =
    BOXROOT_FORCE_REMOTE
    || (BOXROOT_MULTITHREAD
        && (BOXROOT_UNLIKELY(remote_dom_id) || !boxroot_domain_lock_held()));
  if (remote || BOXROOT_UNLIKELY(boxroot_free_slot(fl, root)))
    /* remote deallocation or deallocation threshold */
    boxroot_delete_slow(fl, root, remote);
}

void boxroot_modify_debug(boxroot *rootp);
bool boxroot_modify_slow(boxroot *rootp, value new_value);

inline bool boxroot_modify(boxroot *rootp, value new_value)
{
#if defined(BOXROOT_DEBUG) && (BOXROOT_DEBUG == 1)
  boxroot_modify_debug(rootp);
#endif
  if (BOXROOT_UNLIKELY(!boxroot_domain_lock_held())) return 0;
  slot_ref s = (slot_ref)*rootp;
  boxroot_fl *fl = Get_pool_header(s);
  if (BOXROOT_LIKELY(fl->class == CLASS_YOUNG)) {
    s->as_value = new_value;
    return 1;
  } else {
    /* We might need to reallocate, but this reallocation happens at
       most once between two minor collections. */
    return boxroot_modify_slow(rootp, new_value);
  }
}

#endif // BOXROOT_H
