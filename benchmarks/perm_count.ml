(* SPDX-License-Identifier: MIT *)
module Choice_config = Choice.Config
module Choice = Choice_config.Choice
open Choice.Syntax

let rec insert : type a . a -> a list -> a list Choice.t =
  fun elt xs -> match xs with
  | [] -> Choice.return [elt]
  | x :: xs ->
    Choice.choice
      (Choice.return (elt :: x :: xs))
      (let+ xs' = insert elt xs in x :: xs')

let rec permutation : type a . a list -> a list Choice.t = function
  | [] -> Choice.return []
  | x :: xs ->
    let* xs' = permutation xs in
    insert x xs'

(* (range n) is [0; ..; n-1] *)
let range n =
  let rec loop acc n =
    if n < 0 then acc
    else loop (n :: acc) (n - 1)
  in loop [] (n - 1)

let debug = false

(* the number could be large, so count it as an int64 *)
let count_permutations n =
  let counter = ref Int64.zero in
  let input = range n in
  let perm = permutation input in
  Choice.run perm
    (fun li ->
       if debug then begin
         List.iter (Printf.printf "%d ") li; print_newline ();
       end;
       counter := Int64.succ !counter);
  !counter

let n =
  try int_of_string (Sys.getenv "N")
  with _ ->
    Printf.ksprintf failwith "We expected an environment variable N with an integer value."

let rec fact n = if n = 0 then 1L else Int64.(mul (of_int n) (fact (n - 1)))

let () =
  Ref.Config.Ref.setup ();
  Printf.printf "%s: %!" Ref.Config.implem_name;
  let before = Unix.gettimeofday () in
  let count = count_permutations n in
  let after = Unix.gettimeofday () in
  Printf.printf "%.2fs\n%!" (after -. before);
  assert (count = fact n);
  ignore (Sys.opaque_identity count);
  if Ref.Config.show_stats then
    Ref.Config.Ref.print_stats ();
  Ref.Config.Ref.teardown ();
