external time_ext : unit -> (int64 [@unboxed]) = "caml_time_counter" "caml_time_counter"
let time () = (Int64.to_float (time_ext ())) /. 1_000_000_000.
