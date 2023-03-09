/* SPDX-License-Identifier: MIT */
#ifndef BITMAP_BOXROOT_H
#define BITMAP_BOXROOT_H

#include <caml/mlvalues.h>

typedef struct bitmap_boxroot_private* bitmap_boxroot;

/* `bitmap_boxroot_create(v)` allocates a new boxroot initialised to the
   value `v`. This value will be considered as a root by the OCaml GC
   as long as the boxroot lives or until it is modified. A return
   value of `NULL` indicates a failure of allocation of the backing
   store. */
bitmap_boxroot bitmap_boxroot_create(value);

/* `bitmap_boxroot_get(r)` returns the contained value, subject to the usual
   discipline for non-rooted values. `bitmap_boxroot_get_ref(r)` returns a
   pointer to a memory cell containing the value kept alive by `r`,
   that gets updated whenever its block is moved by the OCaml GC. The
   pointer becomes invalid after any call to `bitmap_boxroot_delete(r)` or
   `bitmap_boxroot_modify(&r,v)`. The argument must be non-null. */
inline value bitmap_boxroot_get(bitmap_boxroot r) { return *(value *)r; }
inline value const * bitmap_boxroot_get_ref(bitmap_boxroot r) {
  return (value *)r;
}

/* `bitmap_boxroot_delete(r)` deallocates the boxroot `r`. The value is no
   longer considered as a root by the OCaml GC. The argument must be
   non-null. Domain lock must be held. */
void bitmap_boxroot_delete(bitmap_boxroot);

/* `bitmap_boxroot_modify(&r,v)` changes the value kept alive by the boxroot
   `r` to `v`. It is equivalent to the following:
   ```
   bitmap_boxroot_delete(r);
   r = bitmap_boxroot_create(v);
   ```
   In particular, the root can be reallocated. However, unlike
   `bitmap_boxroot_create`, `bitmap_boxroot_modify` never fails, so `r` is
   guaranteed to be non-NULL afterwards. In addition, `bitmap_boxroot_modify`
   is more efficient. Indeed, the reallocation, if needed, occurs at
   most once between two minor collections. */
void bitmap_boxroot_modify(bitmap_boxroot *, value);

/* The behaviour of the above functions is well-defined only after the
   allocator has been initialised with `bitmap_boxroot_setup`, which must be
   called after OCaml startup, and before it has released its
   resources with `bitmap_boxroot_teardown`. */
int bitmap_boxroot_setup();
void bitmap_boxroot_teardown();

/* Show some statistics on the standard output. */
void bitmap_boxroot_print_stats();

#endif // BITMAP_BOXROOT_H
