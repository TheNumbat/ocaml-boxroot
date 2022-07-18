/* SPDX-License-Identifier: MIT */
#ifndef OCAML_HOOKS_H
#define OCAML_HOOKS_H

#include <caml/mlvalues.h>
#include "platform.h"

#if OCAML_MULTICORE

inline int boxroot_domain_lock_held(int dom_id)
{
  caml_domain_state *dom_st = Caml_state_opt;
  return BOXROOT_LIKELY(dom_st != NULL)
    && BOXROOT_LIKELY(dom_st->id == dom_id);
}

inline int boxroot_domain_lock_held_any()
{
  return BOXROOT_LIKELY(Caml_state_opt != NULL);
}

#else

/* 0 when the master lock is held, 1 otherwise */
extern _Thread_local int boxroot_thread_has_lock;
void boxroot_enter_blocking_section(void);
void boxroot_leave_blocking_section(void);

/* We need a way to handle concurrent mutations of
   [caml_enter/leave_blocking_section_hook]. They are only overwritten
   once at systhreads init (we assume that no piece of code in the
   OCaml ecosystem is as insane as the present one). We allow
   [boxroot_thread_has_lock] to be falsely 0, but not to be falsely 1.
   If it is falsely 1, then it means that
   [caml_leave_blocking_section_hook] has been overwritten and that
   the present thread has already seen this write.
 */
/* from <caml/signals.h> */
CAMLextern void (*caml_leave_blocking_section_hook)(void);
#define boxroot_hooks_valid()                                           \
  (caml_leave_blocking_section_hook == boxroot_leave_blocking_section)

inline int boxroot_domain_lock_held(int dom_id)
{
  (void)dom_id;
  return  BOXROOT_LIKELY(boxroot_thread_has_lock)
    && BOXROOT_LIKELY(boxroot_hooks_valid());
}

inline int boxroot_domain_lock_held_any()
{
  return boxroot_domain_lock_held(0);
}

#endif

#ifdef CAML_INTERNALS
#include <caml/roots.h>
#include <caml/signals.h>
#include <caml/version.h>

#if OCAML_MULTICORE

#define CALL_GC_ACTION(action, data, v, p) action(data, v, p)
#define Add_to_ref_table(dom_st, p)                   \
  Ref_table_add(&dom_st->minor_tables->major_ref, p);

#else

#define CALL_GC_ACTION(action, data, v, p) do {       \
    action(v, p);                                     \
    (void)data;                                       \
  } while (0)
#define Add_to_ref_table(dom_st, p) add_to_ref_table(dom_st->ref_table, p)

#endif // OCAML_MULTICORE

typedef void (*boxroot_scanning_callback) (scanning_action action,
                                           int only_young, void *data);

void boxroot_setup_hooks(boxroot_scanning_callback scanning,
                         caml_timing_hook domain_termination);

int boxroot_in_minor_collection();

#if !OCAML_MULTICORE

/* Used to regularly check that the hooks have not been overwritten.
   If they have, we reinstall them. Assumes that only systhreads
   modifies them.*/
void boxroot_check_thread_hooks();

#endif // !OCAML_MULTICORE

#endif // CAML_INTERNALS

#endif // OCAML_HOOKS_H
