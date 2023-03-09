(* SPDX-License-Identifier: MIT *)
type 'a t
external create : 'a -> 'a t         = "dll_boxroot_ref_create" [@@noalloc]
external get : 'a t -> 'a            = "dll_boxroot_ref_get" [@@noalloc]
external modify : 'a t array -> int -> 'a -> unit = "dll_boxroot_ref_modify" [@@noalloc]
external delete : 'a t -> unit       = "dll_boxroot_ref_delete" [@@noalloc]

external setup : unit -> unit = "dll_boxroot_ref_setup"
external teardown : unit -> unit = "dll_boxroot_ref_teardown"

external print_stats : unit -> unit = "dll_boxroot_stats"
