(* SPDX-License-Identifier: MIT *)
type impl = {
  fixpoint: (float -> float) -> float -> float;
  setup: unit -> unit;
  teardown: unit -> unit;
  stats: unit -> unit;
}

(* OCaml, no indirection *)
let rec ocaml_fixpoint f x =
  let y = f x in
  if 0 = Float.compare x y then y
  else ocaml_fixpoint f y

(* OCaml, with indirection *)
let rec ocaml_ref_fixpoint_rec f x =
  let y = ref (!f !x) in
  if 0 = Float.compare !x !y then y
  else ocaml_ref_fixpoint_rec f y

let ocaml_ref_fixpoint f x =
  let fr = ref f in
  let xr = ref x in
  !(ocaml_ref_fixpoint_rec fr xr)

external local_fixpoint : (float -> float) -> float -> float = "local_fixpoint"
external naive_fixpoint : (float -> float) -> float -> float = "naive_fixpoint"
external boxroot_fixpoint : (float -> float) -> float -> float = "boxroot_fixpoint"
external dll_boxroot_fixpoint : (float -> float) -> float -> float = "dll_boxroot_fixpoint"
external rem_boxroot_fixpoint : (float -> float) -> float -> float = "rem_boxroot_fixpoint"
external generational_fixpoint : (float -> float) -> float -> float = "generational_root_fixpoint"
external global_fixpoint : (float -> float) -> float -> float = "global_root_fixpoint"

external boxroot_setup : unit -> unit = "boxroot_setup_caml"
external boxroot_teardown : unit -> unit = "boxroot_teardown_caml"
external boxroot_stats : unit -> unit = "boxroot_stats_caml"

external dll_boxroot_setup : unit -> unit = "dll_boxroot_setup_caml"
external dll_boxroot_teardown : unit -> unit = "dll_boxroot_teardown_caml"
external dll_boxroot_stats : unit -> unit = "dll_boxroot_stats_caml"

external rem_boxroot_setup : unit -> unit = "rem_boxroot_setup_caml"
external rem_boxroot_teardown : unit -> unit = "rem_boxroot_teardown_caml"
external rem_boxroot_stats : unit -> unit = "rem_boxroot_stats_caml"


let local = {
  fixpoint = local_fixpoint;
  setup = ignore;
  teardown = ignore;
  stats = ignore;
}

let ocaml = {
  fixpoint = ocaml_fixpoint;
  setup = ignore;
  teardown = ignore;
  stats = ignore;
}

let ocaml_ref = {
  fixpoint = ocaml_ref_fixpoint;
  setup = ignore;
  teardown = ignore;
  stats = ignore;
}

let generational = {
  fixpoint = generational_fixpoint;
  setup = ignore;
  teardown = ignore;
  stats = ignore;
}

let global = {
  fixpoint = global_fixpoint;
  setup = ignore;
  teardown = ignore;
  stats = ignore;
}

let naive = {
  fixpoint = naive_fixpoint;
  setup = boxroot_setup;
  teardown = boxroot_teardown;
  stats = boxroot_stats;
}

let boxroot = {
  fixpoint = boxroot_fixpoint;
  setup = boxroot_setup;
  teardown = boxroot_teardown;
  stats = boxroot_stats;
}

let dll_boxroot = {
  fixpoint = dll_boxroot_fixpoint;
  setup = dll_boxroot_setup;
  teardown = dll_boxroot_teardown;
  stats = dll_boxroot_stats;
}

let rem_boxroot = {
  fixpoint = rem_boxroot_fixpoint;
  setup = rem_boxroot_setup;
  teardown = rem_boxroot_teardown;
  stats = rem_boxroot_stats;
}

let implementations = [
  "local", local;
  "ocaml", ocaml;
  "ocaml_ref", ocaml_ref;
  "naive", naive;
  "boxroot", boxroot;
  "dll_boxroot", dll_boxroot;
  "rem_boxroot", rem_boxroot;
  "generational", generational;
  "global", global;
]

let impl =
  try List.assoc (Sys.getenv "ROOT") implementations with
  | _ ->
    Printf.eprintf "We expect an environment variable ROOT with value one of [ %s ].\n%!"
      (String.concat " | " (List.map fst implementations));
    exit 2

let n =
  let fail () =
    Printf.eprintf "We expect an environment variable N, whose value \
                    is a positive integer.";
    exit 2
  in
  match int_of_string (Sys.getenv "N") with
  | n when n < 1 -> fail ()
  | n -> n
  | exception _ -> fail ()

let show_stats =
  match Sys.getenv "STATS" with
  | "true" | "1" | "yes" -> true
  | "false" | "0" | "no" -> false
  | _ | exception _ -> false

let () =
  impl.setup ();
  Printf.printf "local_roots(ROOT=%-*s, N=%n): %!"
    (List.fold_left max 0 (List.map String.length (List.map fst implementations)))
    (Sys.getenv "ROOT") n;
  let fixpoint = impl.fixpoint in
  let start_time = Unix.gettimeofday () in
  let num_iter = 100_000_000 / n in
  for _i = 1 to num_iter do
    ignore (fixpoint (fun x -> if truncate x >= n then x else x +. 1.) 1.)
  done;
  let duration = Unix.gettimeofday () -. start_time in
  let time_ns = (duration *. 1E9) /. (float_of_int num_iter) in
  Printf.printf "%8.2fns\n%!" time_ns;
  if show_stats then (impl.stats (); print_newline ());
  impl.teardown ();
