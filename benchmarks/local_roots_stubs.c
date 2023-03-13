/* SPDX-License-Identifier: MIT */
#define CAML_NAME_SPACE
#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <caml/fail.h>
#include <caml/callback.h>
#include <assert.h>
#include <locale.h>

/* a fixpoint-computation function defined using the usual C-FFI style
   for OCaml values, following the "callee roots" convention:
   a function is passed value that may be unrooted, and it is
   responsible for rooting them if it may call the GC. */

int compare_val(value x, value y)
{
  /* Simulate a function that does some actual work. */
  CAMLparam2(x, y);
  CAMLreturn(Double_val(x) == Double_val(y));
}

value local_fixpoint(value f, value x)
{
  CAMLparam2(f, x);
  CAMLlocal1(y);
  y = caml_callback(f,x);
  if (compare_val(x,y)) {
    CAMLreturn(y);
  } else {
    CAMLreturn(local_fixpoint(f, y));
  }
}

/* Naive version with boxroots, callee-roots */

#include "../boxroot/boxroot.h"

int compare_val_naive(value x, value y)
{
  boxroot xr = boxroot_create(x);
  boxroot yr = boxroot_create(y);
  int res = Double_val(boxroot_get(xr)) == Double_val(boxroot_get(yr));
  boxroot_delete(yr);
  boxroot_delete(xr);
  return res;
}

value naive_fixpoint(value f, value x)
{
  boxroot fr = boxroot_create(f);
  boxroot xr = boxroot_create(x);
  boxroot yr = boxroot_create(caml_callback(boxroot_get(fr), boxroot_get(xr)));
  value res;
  if (compare_val_naive(boxroot_get(xr),boxroot_get(yr))) {
    res = boxroot_get(yr);
  } else {
    res = local_fixpoint(boxroot_get(fr), boxroot_get(yr));
  }
  boxroot_delete(yr);
  boxroot_delete(xr);
  boxroot_delete(fr);
  return res;
}


/* a different version that uses our 'boxroot' library to implement
   a "caller roots" convention: a function is passed values that
   have already been rooted into boxroots, and it may itself pass them
   around to its own callee without re-rooting. */

#define MY_PREFIX box
#include "local_roots_gen_boxroot.h"
#undef MY_PREFIX

/* Idem for variants */

#include "../boxroot/dll_boxroot.h"
#define MY_PREFIX dll_box
#include "local_roots_gen_boxroot.h"
#undef MY_PREFIX

#include "../boxroot/bitmap_boxroot.h"
#define MY_PREFIX bitmap_box
#include "local_roots_gen_boxroot.h"
#undef MY_PREFIX

#include "../boxroot/rem_boxroot.h"
#define MY_PREFIX rem_box
#include "local_roots_gen_boxroot.h"
#undef MY_PREFIX

/* This is a variation of the caller-root example using "fake
   boxroots", namely malloced blocks containing generational global
   roots. This should be sensibly slower than boxroot, we are using it
   as a "control" that the benchmark makes sense. */

#define MY_PREFIX generational_

typedef value * generational_root;

static inline value generational_root_get(generational_root b) { return *b; }
static inline value const * generational_root_get_ref (generational_root b) { return b; }

static inline generational_root generational_root_create(value v)
{
  value *b = malloc(sizeof(value));
  *b = v;
  caml_register_generational_global_root(b);
  return b;
}

static inline void generational_root_delete(generational_root b)
{
  caml_remove_generational_global_root(b);
  free(b);
}

static int generational_root_setup() { return 1; }
static void generational_root_teardown() { }
static void generational_root_print_stats() { }

#include "local_roots_gen_boxroot.h"
#undef MY_PREFIX

#define MY_PREFIX global_

typedef value * global_root;

static inline value global_root_get(global_root b) { return *b; }
static inline value const * global_root_get_ref (global_root b) { return b; }

static inline global_root global_root_create(value v)
{
  value *b = malloc(sizeof(value));
  *b = v;
  caml_register_global_root(b);
  return b;
}

static inline void global_root_delete(generational_root b)
{
  caml_remove_global_root(b);
  free(b);
}

static int global_root_setup() { return 1; }
static void global_root_teardown() { }
static void global_root_print_stats() { }

#include "local_roots_gen_boxroot.h"
#undef MY_PREFIX
