(* SPDX-License-Identifier: MIT *)
type 'a t
external create : 'a -> 'a t         = "rem_boxroot_ref_create" [@@noalloc]
external get : 'a t -> 'a            = "rem_boxroot_ref_get" [@@noalloc]
external modify : 'a t array -> int -> 'a -> unit = "rem_boxroot_ref_modify" [@@noalloc]
external delete : 'a t -> unit       = "rem_boxroot_ref_delete" [@@noalloc]

external setup : unit -> unit = "rem_boxroot_ref_setup"
external teardown : unit -> unit = "rem_boxroot_ref_teardown"

external print_stats : unit -> unit = "rem_boxroot_stats"
