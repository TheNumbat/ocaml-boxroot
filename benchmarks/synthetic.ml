(* SPDX-License-Identifier: MIT *)
module Ref_config = Ref.Config
module Time = Ref.Time
module Ref = Ref_config.Ref

(* a synthetic benchmark with tunable parameters.

IMPLEM=boxroot N=6 \
SMALL_ROOTS=1_000 \
YOUNG_RATIO=1 \
LARGE_ROOTS=0 \
SMALL_ROOT_PROMOTION_RATE=0.01 \
LARGE_ROOT_PROMOTION_RATE=0 \
ROOT_SURVIVAL_RATE=1 \
GC_PROMOTION_RATE=0.1 \
GC_SURVIVAL_RATE=0.5 \
./benchmarks/synthetic.exe

*)

let wrong_usage () =
  Printf.eprintf "Expected environment variables:
   N: log_2 of the number of minor generations
   SMALL_ROOTS: the number of small roots allocated per minor collection
   YOUNG_RATIO: the ratio of small roots containing a newly allocated value in the minor heap
   LARGE_ROOTS: the number of large roots allocated (in the major heap) per minor collection
   SMALL_ROOT_PROMOTION_RATE: the survival rate for small roots allocated in the current minor heap
   LARGE_ROOT_PROMOTION_RATE: the survival rate for large roots allocated in the current minor heap
   ROOT_SURVIVAL_RATE: the survival rate for roots that survived a first minor collection

   GC_PROMOTION_RATE: promotion rate of GC-tracked values
   GC_SURVIVAL_RATE: survival rate of GC-tracked values

   Note: because we use fresh non-immediate OCaml values as the content of new roots,
   each SMALL_ROOT or LARGE_ROOT creation consumes some minor-heap space, and in particular
   values of SMALL_ROOT or LARGE_ROOT that are too large may not be satisfiable -- if a minor
   collection happens before the allocation target is reached.
  ";
  exit 5
  (* Remark: instead of allocating fresh values for each small root,
     we could allocate one fresh value at the start of the minor period,
     and then reuse it for all small roots. Currently we still allocate
     several words to store the small root, but that may also be avoidable
     (by storing the words in an array, fresh for the period but
      allocated on the major heap.) *)

let get_param reader param =
  try reader (Sys.getenv param)
  with _ ->
    Printf.eprintf "Environment variable %s missing or invalid.%!" param;
    wrong_usage ()

let old_value =
  let v = Some 42 in
  Gc.minor();
  v

let small_roots_per_minor =
  get_param int_of_string "SMALL_ROOTS"

let young_ratio =
  get_param float_of_string "YOUNG_RATIO"

let large_roots_per_minor =
  get_param int_of_string "LARGE_ROOTS"

let small_root_promotion_rate =
  get_param float_of_string "SMALL_ROOT_PROMOTION_RATE"

let large_root_promotion_rate =
  get_param float_of_string "LARGE_ROOT_PROMOTION_RATE"

let root_survival_rate =
  get_param float_of_string "ROOT_SURVIVAL_RATE"

let gc_promotion_rate =
  get_param float_of_string "GC_PROMOTION_RATE"

let gc_survival_rate =
  get_param float_of_string "GC_SURVIVAL_RATE"

module Dll = struct
  type 'a t = 'a cell option ref
  and 'a cell = {
    parent: 'a t;
    mutable prev: 'a cell;
    mutable next: 'a cell;
    contents: 'a;
  }

  let empty () = ref None

  let push dll contents =
    match !dll with
    | None ->
      let rec cell = {
        parent = dll;
        prev = cell;
        next = cell;
        contents;
      } in
      dll := Some cell
    | Some front ->
      let cell = {
        parent = dll;
        prev = front.prev;
        next = front;
        contents;
      }
      in
      cell.prev.next <- cell;
      cell.next.prev <- cell;
      dll := Some cell

  let filter_inplace dll p =
    match !dll with
    | None -> ()
    | Some front ->
      let rec count acc cell =
        if cell.next == front then acc
        else count (acc + 1) cell.next
      in
      let length = count 1 front in
      let rec loop i ~found_one cell =
        (* found_one: whether at least one cell already satisfied the predicate *)
        if i = length then begin
          if not found_one then dll := None;
        end else begin
          let found_one =
            if p cell.contents then begin
              if not found_one then dll := Some cell;
              true
            end else begin
              cell.prev.next <- cell.next;
              cell.next.prev <- cell.prev;
              found_one
            end in
          loop (i + 1) ~found_one cell.next
        end in
      loop 0 ~found_one:false front
end

let run n =
  Random.init 42;
  let within rate =
    Random.float 1. < rate
  in
  let best_small_count = ref 0 in
  let best_large_count = ref 0 in
  for _runs = 1 to 10 do
    let small_roots = Dll.empty () in
    let large_roots = Dll.empty () in
    let values = Dll.empty () in

    for _minor_generations = 1 to 1 lsl n do
      let small_count = ref 0 in
      let large_count = ref 0 in

      let same_minor_collection =
        let last_minor_free = ref max_int in
        fun () ->
          let minor_free = Gc.get_minor_free () in
          if minor_free < !last_minor_free then begin
            last_minor_free := minor_free;
            true
          end else begin
            last_minor_free := max_int;
            false
          end
      in

      let allocate_small_root () =
        let value =
          if within young_ratio
          then Some !small_count
          else old_value
        in
        let root = Ref.create value in
        incr small_count;
        if within small_root_promotion_rate
        then Dll.push small_roots root
        else Ref.delete root
      in
      let allocate_large_root () =
        let value = Array.make 512 !large_count in
        let root = Ref.create value in
        incr large_count;
        if within large_root_promotion_rate
        then Dll.push large_roots root
        else Ref.delete root
      in
      let allocate_value () =
        let value = Some !small_roots in
        if within gc_promotion_rate
        then Dll.push values value
      in

      (* allocate values until the next minor collection *)
      while same_minor_collection () do
        if !small_count < small_roots_per_minor then allocate_small_root ();
        if !large_count < large_roots_per_minor then allocate_large_root ();
        allocate_value ();
      done;

      best_small_count := max !best_small_count !small_count;
      best_large_count := max !best_large_count !large_count;

      Dll.filter_inplace small_roots (fun root ->
        within root_survival_rate
        || (Ref.delete root; false));

      Dll.filter_inplace large_roots (fun root ->
        within root_survival_rate
        || (Ref.delete root; false));

      Dll.filter_inplace values (fun _ ->
        within gc_survival_rate);

    done;

    (* end of the round: delete all remaining roots *)
    Dll.filter_inplace small_roots (fun root ->
      Ref.delete root; false);
    Dll.filter_inplace large_roots (fun root ->
      Ref.delete root; false);
  done;

  if !best_small_count < small_roots_per_minor then
    Printf.eprintf
      "Warning: we were not able to reach the SMALL_BOXROOTS target (%d),
       we allocated %d small roots on our best collection.\n"
      small_roots_per_minor
      !best_small_count;
  if !best_large_count < large_roots_per_minor then
    Printf.eprintf
      "Warning: we were not able to reach the LARGE_BOXROOTS target (%d),
       we allocated %d large roots on our best collection.\n"
      large_roots_per_minor
      !best_large_count;
  ()

let n =
  try int_of_string (Sys.getenv "N")
  with _ ->
    Printf.ksprintf failwith "We expected an environment variable N with an integer value."

let () =
  Ref.setup ();
  Printf.printf "%s: %!" Ref_config.implem_name;
  let before = Time.time () in
  run n;
  let after = Time.time () in
  Printf.printf "%.2fs\n%!" (after -. before);
  if Ref_config.show_stats then
    Ref.print_stats ();
  Ref.teardown ();
