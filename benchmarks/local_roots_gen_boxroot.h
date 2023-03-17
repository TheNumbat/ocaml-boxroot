/* SPDX-License-Identifier: MIT */
#define CONCAT(g,f) g ## f
#define MY2(g,f) CONCAT(g,f)
#define MY(f) MY2(MY_PREFIX, f)

typedef value const * value_ref;

static int MY(compare_refs)(value_ref x, value_ref y)
{
  /* Simulate a function that does some actual work---nothing to root
     here. */
  return Double_val(*x) == Double_val(*y);
}

MY(root) MY(root_fixpoint_rooted)(value_ref f, MY(root) x)
{
  MY(root) y = MY(root_create)(caml_callback(*f, MY(root_get)(x)));
  if (MY(compare_refs)(MY(root_get_ref)(x), MY(root_get_ref)(y))) {
    MY(root_delete)(x);
    return y;
  } else {
    MY(root_delete)(x);
    return MY(root_fixpoint_rooted)(f, y);
  }
}

value MY(root_fixpoint)(value f, value x)
{
  MY(root) f_root = MY(root_create)(f);
  MY(root) y = MY(root_fixpoint_rooted)(MY(root_get_ref)(f_root),
                                        MY(root_create)(x));
  value v = MY(root_get)(y);
  MY(root_delete)(y);
  MY(root_delete)(f_root);
  return v;
}

/* TODO: it is annoying to have to copy this code in several
   libraries, maybe we should provide an OCaml library that simply
   re-exports the boxroot.h interface within OCaml. */
value MY(root_setup_caml)(value unit)
{
  if (!MY(root_setup)()) caml_failwith("root_setup_caml");
  return unit;
}

value MY(root_teardown_caml)(value unit)
{
  MY(root_teardown)();
  return unit;
}

value MY(root_stats_caml)(value unit)
{
  char *old_locale = setlocale(LC_NUMERIC, NULL);
  setlocale(LC_NUMERIC, "en_US.UTF-8");
  MY(root_print_stats)();
  setlocale(LC_NUMERIC, old_locale);
  return unit;
}
