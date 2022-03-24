#ifndef BOXROOT_H
#define BOXROOT_H

#define CAML_NAME_SPACE

#include <caml/mlvalues.h>
#include <caml/minor_gc.h>
#include <caml/address_class.h>
#include "ocaml_hooks.h"
#include "platform.h"

typedef struct boxroot_private* boxroot;

/* `boxroot_create(v)` allocates a new boxroot initialised to the
   value `v`. This value will be considered as a root by the OCaml GC
   as long as the boxroot lives or until it is modified. A return
   value of `NULL` indicates a failure of allocation of the backing
   store. */
inline boxroot boxroot_create(value);

/* `boxroot_get(r)` returns the contained value, subject to the usual
   discipline for non-rooted values. `boxroot_get_ref(r)` returns a
   pointer to a memory cell containing the value kept alive by `r`,
   that gets updated whenever its block is moved by the OCaml GC. The
   pointer becomes invalid after any call to `boxroot_delete(r)` or
   `boxroot_modify(&r,v)`. The argument must be non-null. */
inline value boxroot_get(boxroot r) { return *(value *)r; }
inline value const * boxroot_get_ref(boxroot r) { return (value *)r; }

/* `boxroot_delete(r)` deallocates the boxroot `r`. The value is no
   longer considered as a root by the OCaml GC. The argument must be
   non-null. */
inline void boxroot_delete(boxroot);

/* `boxroot_modify(&r,v)` changes the value kept alive by the boxroot
   `r` to `v`. It is equivalent to the following:
   ```
   boxroot_delete(r);
   r = boxroot_create(v);
   ```
   In particular, the root can be reallocated. However, unlike
   `boxroot_create`, `boxroot_modify` never fails, so `r` is
   guaranteed to be non-NULL afterwards. In addition, `boxroot_modify`
   is more efficient. Indeed, the reallocation, if needed, occurs at
   most once between two minor collections. */
void boxroot_modify(boxroot *, value);


/* The behaviour of the above functions is well-defined only after the
   allocator has been initialised with `boxroot_setup`, which must be
   called after OCaml startup, and before it has released its
   resources with `boxroot_teardown`, which can be called after OCaml
   shutdown. */
int boxroot_setup();
void boxroot_teardown();

/* Show some statistics on the standard output. */
void boxroot_print_stats();


/* Private implementation */

typedef struct {
  void *next;
  int alloc_count;
} boxroot_fl;

extern boxroot_fl *boxroot_current_fl;

/* The following definition has smaller code size, and should have
   better branch prediction than Is_young. */
#if OCAML_MULTICORE
#define Is_young2(val)                                                \
  ((uintnat)(val) - (uintnat)caml_minor_heaps_base <                  \
   (uintnat)caml_minor_heaps_end - (uintnat)caml_minor_heaps_base)
#else
#define Is_young2(val)                                                \
  ((uintnat)(val) - (uintnat)Caml_state->young_start <=               \
   (uintnat)Caml_state->young_end - (uintnat)Caml_state->young_start)
#endif // OCAML_MULTICORE

#define Is_young_block(v) (Is_young2(v) && LIKELY(Is_block(v)))

#if (defined(ENABLE_BOXROOT_MUTEX) && (ENABLE_BOXROOT_MUTEX == 1)) || \
  (defined(BOXROOT_DEBUG) && (BOXROOT_DEBUG == 1))
#define BOXROOT_NO_INLINE
#endif

#ifdef BOXROOT_NO_INLINE

boxroot boxroot_create_impl(value v);

inline boxroot boxroot_create(value v)
{
  return boxroot_create_impl(v);
}

void boxroot_delete_impl(boxroot root);

inline void boxroot_delete(boxroot root)
{
  return boxroot_delete_impl(root);
}

#else

boxroot boxroot_create_slow(value);

inline boxroot boxroot_create(value init)
{
  boxroot_fl *fl = boxroot_current_fl;
  if (UNLIKELY(fl == NULL)) goto out_slow;
  void *new_root = fl->next;
  // is pool full?
  if (UNLIKELY(new_root == fl)) goto out_slow;
  fl->next = *((void **)new_root);
  fl->alloc_count++;
  *((value *)new_root) = init;
  return (boxroot)new_root;
out_slow:
  return boxroot_create_slow(init);
}

/* Log of the size of the pools (12 = 4KB, an OS page).
   Recommended: 14. */
#define POOL_LOG_SIZE 14
#define POOL_SIZE ((size_t)1 << POOL_LOG_SIZE)
/* Take the slow path on deallocation every DEALLOC_THRESHOLD_SIZE
   deallocations. */
#define DEALLOC_THRESHOLD_SIZE_LOG 7 // 128
#define DEALLOC_THRESHOLD_SIZE ((int)1 << DEALLOC_THRESHOLD_SIZE_LOG)

void boxroot_try_demote_pool(boxroot_fl *p);

#define Get_freelist(s) ((boxroot_fl *)((uintptr_t)s & ~((uintptr_t)POOL_SIZE - 1)))

inline void boxroot_delete(boxroot root)
{
  void **s = (void **)root;
  boxroot_fl *fl = Get_freelist(s);
  *s = (void *)fl->next;
  fl->next = s;
  int alloc_count = --fl->alloc_count;
  if (UNLIKELY((alloc_count & (DEALLOC_THRESHOLD_SIZE - 1)) == 0
               && fl != boxroot_current_fl)) {
    boxroot_try_demote_pool(fl);
  }
}

#endif // BOXROOT_NO_INLINE

#endif // BOXROOT_H
