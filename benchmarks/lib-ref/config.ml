(* SPDX-License-Identifier: MIT *)
module type Ref = sig
  type 'a t
  val create : 'a -> 'a t
  val get : 'a t -> 'a
  val modify : 'a t array -> int -> 'a -> unit
  val delete : 'a t -> unit

  val setup : unit -> unit
  val teardown : unit -> unit
  val print_stats : unit -> unit
end

let choose_implem var implementations =
  let implem_name =
    try Sys.getenv var
    with _ ->
      Printf.ksprintf failwith "The environment variable %s must be defined." var
  in
  let implem =
    try List.assoc implem_name implementations
    with Not_found ->
      Printf.ksprintf failwith "Invalid environment variable %s=%s, expected one of:\n%s"
        var implem_name
        (String.concat ", " (List.map fst implementations))
  in
  implem_name, implem

let implementations : (string * (module Ref)) list = [
  "global", (module Global_ref);
  "generational", (module Generational_ref);
  "boxroot", (module Boxroot_ref);
  "dll_boxroot", (module Dll_boxroot_ref);
  "bitmap_boxroot", (module Bitmap_boxroot_ref);
  "rem_boxroot", (module Rem_boxroot_ref);
  "gc", (module Gc_ref);
  "ocaml_ref", (module Ocaml_ref);
  "ocaml", (module Ocaml);
]

let implem_name, implem_module =
  choose_implem "REF" implementations

let show_stats =
  match Sys.getenv "STATS" with
  | exception _ -> false
  | "1" | "true" | "yes" -> true
  | "0" | "false" | "no" -> false
  | other ->
    Printf.eprintf "Unknown value %S for the environment variable STATS.\n\
                    Expected 'true' or 'false'.\n%!" other;
    false

module Ref = struct
  include (val implem_module : Ref)

  (* count the number of live references;
     this is useful to check that user programs
     respect the linearity discipline on Ref values,
     that they do not leak un-deleted values. *)
  let live = ref 0

  let create v =
    incr live;
    create v

  let delete r =
    delete r;
    decr live

  let consume (r : 'a t) : 'a =
    let v = get r in
    delete r;
    v

  let live () = !live
end
