(* SPDX-License-Identifier: MIT *)
type 'a t
external create : 'a -> 'a t         = "gc_ref_create"
external get : 'a t -> 'a            = "gc_ref_get" [@@noalloc]
external modify : 'a t array -> int -> 'a -> unit = "gc_ref_modify" [@@noalloc]
external delete : 'a t -> unit       = "gc_ref_delete" [@@noalloc]

external setup : unit -> unit = "boxroot_ref_setup"
external teardown : unit -> unit = "boxroot_ref_teardown"
external print_stats : unit -> unit = "boxroot_stats"
