(* SPDX-License-Identifier: MIT *)
type 'a t
external create : 'a -> 'a t         = "bitmap_boxroot_ref_create" [@@noalloc]
external get : 'a t -> 'a            = "bitmap_boxroot_ref_get" [@@noalloc]
external modify : 'a t array -> int -> 'a -> unit = "bitmap_boxroot_ref_modify" [@@noalloc]
external delete : 'a t -> unit       = "bitmap_boxroot_ref_delete" [@@noalloc]

external setup : unit -> unit = "bitmap_boxroot_ref_setup"
external teardown : unit -> unit = "bitmap_boxroot_ref_teardown"

external print_stats : unit -> unit = "bitmap_boxroot_stats"
