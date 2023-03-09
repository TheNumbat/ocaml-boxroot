(* SPDX-License-Identifier: MIT *)
(* This benchmark is a direct port of gc-roots/globroots.ml from the OCaml testsuite
     https://github.com/ocaml/ocaml/blob/trunk/testsuite/tests/gc-roots/globroots.ml

   make -C .. benchmarks/globroots.exe \
   && REF=global CHOICE=persistent N=500_000 ./globroots.exe
*)

module MakeTest(G: Ref.Config.Ref) = struct

  let size = 1024

  let vals = Array.init size Int.to_string

  let a = Array.init size (fun i -> G.create (Int.to_string i))

  let tick = ref (0,1)

  let check () =
    for i = 0 to size - 1 do
      if G.get a.(i) <> vals.(i) then begin
        print_string "Error on "; print_int i; print_string ": ";
        print_string (String.escaped (G.get a.(i))); print_newline()
      end
    done

  let change () =
    (* Make sure at least one minor allocation takes place between any
       two collections. *)
    tick := (match !tick with (a,b) -> (b,a));
    match Random.int 37 with
    | 0 ->
        Gc.full_major()
    | 1|2|3|4 ->
        Gc.minor()
    | 5|6|7|8|9|10|11|12 ->             (* update with young value *)
        let i = Random.int size in
        G.modify a i (Int.to_string i)
    | 13|14|15|16|17|18|19|20 ->        (* update with old value *)
        let i = Random.int size in
        G.modify a i vals.(i)
    | 21|22|23|24|25|26|27|28 ->        (* re-register young value *)
        let i = Random.int size in
        G.delete a.(i);
        a.(i) <- G.create (Int.to_string i)
    | (*29|30|31|32|33|34|35|36*) _ ->  (* re-register old value *)
        let i = Random.int size in
        G.delete a.(i);
        a.(i) <- G.create vals.(i)

  let test n =
    for _i = 1 to n do
      change();
    done
end

let n =
  try int_of_string (Sys.getenv "N")
  with _ ->
    Printf.ksprintf failwith "We expected an environment variable N with an integer value."

let _ =
  Ref.Config.Ref.setup ();
  Printf.printf "%s: %!" Ref.Config.implem_name;
  let module Test = MakeTest(Ref.Config.Ref) in
  let before = Ref.Time.time () in
  Test.test n;
  let after = Ref.Time.time () in
  Printf.printf "%.2fs\n%!" (after -. before);
  if Ref.Config.show_stats then
    Ref.Config.Ref.print_stats ();
  Ref.Config.Ref.teardown ();
