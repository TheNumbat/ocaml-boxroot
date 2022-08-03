/* SPDX-License-Identifier: MIT */
pub type Value = isize;
pub type BoxRoot = std::ptr::NonNull<std::cell::UnsafeCell<Value>>;

extern "C" {
    pub fn boxroot_create(v: Value) -> Option<BoxRoot>;
    pub fn boxroot_get(br: BoxRoot) -> Value;
    pub fn boxroot_get_ref(br: BoxRoot) -> *const Value;
    pub fn boxroot_delete(br: BoxRoot);
    pub fn boxroot_modify(br: *mut BoxRoot, v: Value) -> bool;
    pub fn boxroot_teardown();
    pub fn boxroot_status() -> Status;
    pub fn boxroot_print_stats();

    /// obsolete
    pub fn boxroot_setup();
}

#[repr(C)]
#[non_exhaustive]
pub enum Status {
  NotSetup,
  Running,
  ToreDown,
  Invalid
}

// Just a test to verify that it compiles and links right
// Run with: cargo test --features "link-ocaml-runtime-and-dummy-program"
#[cfg(test)]
mod tests {
    use crate::{
        boxroot_create, boxroot_delete, boxroot_get, boxroot_get_ref, boxroot_modify,
        boxroot_teardown,
    };

    extern "C" {
        pub fn caml_startup(argv: *const *const i8);
        pub fn caml_shutdown();
    }

    #[test]
    fn it_works() {
        unsafe {
            let arg0 = "ocaml\0".as_ptr() as *const i8;
            let c_args = vec![arg0, core::ptr::null()];

            caml_startup(c_args.as_ptr());

            let mut br = boxroot_create(1).unwrap();
            let v1 = *boxroot_get_ref(br);

            boxroot_modify(&mut br, 2);
            let v2 = boxroot_get(br);

            boxroot_delete(br);

            assert_eq!(v1, 1);
            assert_eq!(v2, 2);

            boxroot_teardown();

            caml_shutdown();
        }
    }
}
