(* SPDX-License-Identifier: MIT *)
type 'a t
external create : 'a -> 'a t         = "boxroot_ref_create" [@@noalloc]
external get : 'a t -> 'a            = "boxroot_ref_get" [@@noalloc]
external modify : 'a t array -> int -> 'a -> unit = "boxroot_ref_modify" [@@noalloc]
external delete : 'a t -> unit       = "boxroot_ref_delete" [@@noalloc]

external setup : unit -> unit = "boxroot_ref_setup"
external teardown : unit -> unit = "boxroot_ref_teardown"

external print_stats : unit -> unit = "boxroot_stats"
